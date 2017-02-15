/*
 * @BEGIN LICENSE
 *
 * Psi4: an open-source quantum chemistry software package
 *
 * Copyright (c) 2007-2017 The Psi4 Developers.
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * @END LICENSE
 */


#include "psi4/lib3index/3index.h"
#include "psi4/libpsio/psio.hpp"
#include "psi4/libpsio/psio.h"
#include "psi4/libpsio/aiohandler.h"
#include "psi4/libqt/qt.h"
#include "psi4/psi4-dec.h"
#include "psi4/psifiles.h"
#include "psi4/libmints/sieve.h"
#include "psi4/libiwl/iwl.hpp"
#include "jk.h"
#include "jk_independent.h"
#include "link.h"
#include "direct_screening.h"
#include "cubature.h"
#include "points.h"
#include "psi4/libmints/matrix.h"
#include "psi4/libmints/basisset.h"
#include "psi4/libmints/twobody.h"
#include "psi4/libmints/integral.h"
#include "psi4/lib3index/cholesky.h"

#include <sstream>
#include "psi4/libparallel/ParallelPrinter.h"
#ifdef _OPENMP
#include <omp.h>
#endif

using namespace std;
using namespace psi;

namespace psi {
DirectJK::DirectJK(std::shared_ptr<BasisSet> primary) :
   JK(primary)
{
    common_init();
}
DirectJK::~DirectJK()
{
}
void DirectJK::common_init()
{
    df_ints_num_threads_ = 1;
    #ifdef _OPENMP
        df_ints_num_threads_ = omp_get_max_threads();
    #endif
}
void DirectJK::print_header() const
{
    if (print_) {
        outfile->Printf( "  ==> DirectJK: Integral-Direct J/K Matrices <==\n\n");

        outfile->Printf( "    J tasked:          %11s\n", (do_J_ ? "Yes" : "No"));
        outfile->Printf( "    K tasked:          %11s\n", (do_K_ ? "Yes" : "No"));
        outfile->Printf( "    wK tasked:         %11s\n", (do_wK_ ? "Yes" : "No"));
        if (do_wK_)
            outfile->Printf( "    Omega:             %11.3E\n", omega_);
        outfile->Printf( "    Integrals threads: %11d\n", df_ints_num_threads_);
        //outfile->Printf( "    Memory (MB):       %11ld\n", (memory_ *8L) / (1024L * 1024L));
        outfile->Printf( "    Schwarz Cutoff:    %11.0E\n\n", cutoff_);
    }
}
void DirectJK::preiterations()
{
    sieve_ = std::shared_ptr<ERISieve>(new ERISieve(primary_, cutoff_));
}
void DirectJK::compute_JK()
{
    std::shared_ptr<IntegralFactory> factory(new IntegralFactory(primary_,primary_,primary_,primary_));

    if (do_wK_) {
        std::vector<std::shared_ptr<TwoBodyAOInt> > ints;
        for (int thread = 0; thread < df_ints_num_threads_; thread++) {
            ints.push_back(std::shared_ptr<TwoBodyAOInt>(factory->erf_eri(omega_)));
        }
        // TODO: Fast K algorithm
        if (do_J_) {
            build_JK(ints,D_ao_,J_ao_,wK_ao_);
        } else {
            std::vector<std::shared_ptr<Matrix> > temp;
            for (size_t i = 0; i < D_ao_.size(); i++) {
                temp.push_back(std::shared_ptr<Matrix>(new Matrix("temp", primary_->nbf(), primary_->nbf())));
            }
            build_JK(ints,D_ao_,temp,wK_ao_);
        }
    }

    if (do_J_ || do_K_) {
        std::vector<std::shared_ptr<TwoBodyAOInt> > ints;
        ints.push_back(std::shared_ptr<TwoBodyAOInt>(factory->eri()));
        for (int thread = 1; thread < df_ints_num_threads_; thread++) {
            if(ints[0]->cloneable())
                ints.push_back(std::shared_ptr<TwoBodyAOInt>(ints[0]->clone()));
            else
                ints.push_back(std::shared_ptr<TwoBodyAOInt>(factory->eri()));
        }
        if (do_J_ && do_K_) {
            build_JK(ints,D_ao_,J_ao_,K_ao_);
        } else if (do_J_) {
            std::vector<std::shared_ptr<Matrix> > temp;
            for (size_t i = 0; i < D_ao_.size(); i++) {
                temp.push_back(std::shared_ptr<Matrix>(new Matrix("temp", primary_->nbf(), primary_->nbf())));
            }
            build_JK(ints,D_ao_,J_ao_,temp);
        } else {
            std::vector<std::shared_ptr<Matrix> > temp;
            for (size_t i = 0; i < D_ao_.size(); i++) {
                temp.push_back(std::shared_ptr<Matrix>(new Matrix("temp", primary_->nbf(), primary_->nbf())));
            }
            build_JK(ints,D_ao_,temp,K_ao_);
        }
    }
}
void DirectJK::postiterations()
{
    sieve_.reset();
}
void DirectJK::build_JK(std::vector<std::shared_ptr<TwoBodyAOInt> >& ints,
                        std::vector<std::shared_ptr<Matrix> >& D,
                        std::vector<std::shared_ptr<Matrix> >& J,
                        std::vector<std::shared_ptr<Matrix> >& K)
{
    const int nthread = ints.size();
    const size_t nshell = primary_->nshell();
    const int maxam = primary_->max_am();
    const size_t nbf = primary_->nbf();
    const size_t nbf2 = nbf * nbf;
    const size_t nmat_J = ( do_J_ ? J.size() : 0 );
    const size_t nmat_K = ( do_K_ ? K.size() : 0 );

    const size_t worksize_J = ( do_J_ ? nbf2 : 0 );
    const size_t worksize_K = ( do_K_ ? nbf2 : 0 );
    const size_t worksize = worksize_J + worksize_K;

    // allocate and zero temporary workspaces
    std::unique_ptr<double[]> work(new double[worksize*nthread]);
    double * workptr = work.get();
    memset(workptr, 0, worksize*nthread*sizeof(double));


    // create PQ blocks
    // only one block per batch
    std::vector<ShellPairBlock> PQ_blocks;

    for(int P = 0; P < nshell; P++)
    for(int Q = 0; Q <= P; Q++)
    {
        if(sieve_->shell_pair_significant(P, Q))
            PQ_blocks.push_back({{P, Q}});
    }

    // sort shells by am
    std::vector<std::vector<int>> sorted_shells(maxam+1);
    for(int P = 0; P < nshell; P++)
        sorted_shells[primary_->shell(P).am()].push_back(P);

    const size_t nblocks_PQ = PQ_blocks.size();

    #pragma omp parallel for num_threads(nthread) schedule(guided)
    for (int PQblock_idx = 0; PQblock_idx < nblocks_PQ; ++PQblock_idx)
    {
        #ifdef _OPENMP
            const int thread = omp_get_thread_num();
        #else
            const int thread = 0;
        #endif

        double * const mywork = workptr + thread*worksize;
        double * const my_J = mywork;
        double * const my_K = mywork + worksize_J;

        const auto & PQblock = PQ_blocks[PQblock_idx];

        const int P = PQblock[0].first;
        const int Q = PQblock[0].second;
        const auto & shellP = primary_->shell(P);
        const auto & shellQ = primary_->shell(Q);
        const int nP = shellP.nfunction();
        const int nQ = shellQ.nfunction();
        const int Pstart = shellP.function_index();
        const int Qstart = shellQ.function_index();
        const int Pend = Pstart + nP;
        const int Qend = Qstart + nQ;
        const int PQ = (P*(P+1))/2 + Q;

        // generate the batches for RS
        std::vector<ShellPairBlock> RS_blocks;

        for(int am1 = 0; am1 <= maxam; am1++)
        for(int am2 = 0; am2 <= maxam; am2++)
        {
            ShellPairBlock blk;

            for(size_t i = 0; i < sorted_shells[am1].size(); i++)
            for(size_t j = 0; j < sorted_shells[am2].size(); j++)
            {
                int R = sorted_shells[am1][i];
                int S = sorted_shells[am2][j];

                const int RS = (R*(R+1))/2 + S;
                if(S <= R && RS <= PQ
                          && sieve_->shell_pair_significant(R, S)
                          && sieve_->shell_significant(P,Q,R,S))
                {
                    blk.push_back({R, S});
                    if(blk.size() == 32)
                    {
                        RS_blocks.push_back(blk);
                        blk.clear();
                    }
                }
            }

            if(blk.size())
                RS_blocks.push_back(std::move(blk));
        }

        const size_t nblocks_RS = RS_blocks.size();


        for (int RSblock_idx = 0; RSblock_idx < nblocks_RS; ++RSblock_idx)
        {
            const auto & RSblock = RS_blocks[RSblock_idx];
            const size_t nRS = RSblock.size();

            // compute the blocks
            ints[thread]->compute_shell_blocks(PQblock, RSblock);
            const double * buffer = ints[thread]->buffer();

            for(size_t RS_idx = 0; RS_idx < nRS; RS_idx++)
            {
                // unpack what indices we are doing
                const int R = RSblock[RS_idx].first;
                const int S = RSblock[RS_idx].second;

                const auto & shellR = primary_->shell(R);
                const auto & shellS = primary_->shell(S);
                const int nR = shellR.nfunction();
                const int nS = shellS.nfunction();
                const int Rstart = shellR.function_index();
                const int Sstart = shellS.function_index();
                const int Rend = Rstart + nR;
                const int Send = Sstart + nS;
                const int RS = (R*(R+1))/2 + S;

                double fac = 1.0;
                if(P == Q) fac *= 0.5;
                if(R == S) fac *= 0.5;
                if(PQ == RS) fac *= 0.5;

                for (int p = Pstart, index = 0; p < Pend; p++)
                for (int q = Qstart           ; q < Qend; q++)
                for (int r = Rstart           ; r < Rend; r++)
                for (int s = Sstart           ; s < Send; s++, index++)
                {
                    const int pq = (p*(p+1))/2 + q;
                    const int rs = (r*(r+1))/2 + s;

                    const int nbf_p = p*nbf;
                    const int nbf_q = q*nbf;
                    const int nbf_r = r*nbf;
                    const int nbf_s = s*nbf;

                    double val = fac*buffer[index];

                    if (do_J_)
                    {
                        for (int i = 0; i < nmat_J; i++)
                        {
                            double const * const Dp = D[i]->get_pointer();
                            double * const Jp = my_J + i*nbf2;
                            Jp[nbf_p + q] += (Dp[nbf_r+s] + Dp[nbf_s+r])*val;
                            Jp[nbf_q + p] += (Dp[nbf_r+s] + Dp[nbf_s+r])*val;
                            Jp[nbf_r + s] += (Dp[nbf_p+q] + Dp[nbf_q+p])*val;
                            Jp[nbf_s + r] += (Dp[nbf_p+q] + Dp[nbf_q+p])*val;
                        }
                    }

                    if (do_K_)
                    {
                        for (int i = 0; i < nmat_K; i++)
                        {
                            double const * const Dp = D[i]->get_pointer();
                            double * const Kp = my_K + i*nbf2;

                            Kp[nbf_p + r] += Dp[nbf_q+s]*val;
                            Kp[nbf_q + r] += Dp[nbf_p+s]*val;
                            Kp[nbf_p + s] += Dp[nbf_q+r]*val;
                            Kp[nbf_q + s] += Dp[nbf_p+r]*val;

                            Kp[nbf_r + p] += Dp[nbf_s+q]*val;
                            Kp[nbf_s + p] += Dp[nbf_r+q]*val;
                            Kp[nbf_r + q] += Dp[nbf_s+p]*val;
                            Kp[nbf_s + q] += Dp[nbf_r+p]*val;
                        }
                    }
                }

                buffer += nP*nQ*nR*nS;

            }
        }
    }


    // commit to the final matrix
    // This does a quick and dirty tree reduction. The matrices
    // accumulate at the end of the workspace
    size_t cthreads = nthread;
    while(cthreads > 2)
    {

        #pragma omp parallel for num_threads(cthreads/2)
        for(size_t i = 0; i < cthreads/2; i++)
        {
            double * const src = workptr + i*worksize;
            double * const dest = workptr + (i+(cthreads/2))*worksize;

            for(size_t n = 0; n < (nmat_J+nmat_K)*nbf2; n++)
                dest[n] += src[n];
        }
        workptr += (cthreads/2)*worksize;
        cthreads = (cthreads % 2 ? (cthreads/2+1) : (cthreads/2));
    }

    if(do_J_)
    {
        for(int i = 0; i < nmat_J; i++)
        {
            double const * const my_J1 = workptr;
            double const * const my_J2 = workptr + worksize;
            double * const Jp = J[i]->get_pointer();

            for(size_t n = 0; n < nbf2; n++)
                Jp[n] = my_J1[n] + my_J2[n];
        }
    }

    if(do_K_)
    {
        for(int i = 0; i < nmat_K; i++)
        {
            double const * const my_K1 = workptr + worksize_J;
            double const * const my_K2 = workptr + worksize + worksize_J;
            double * const Kp = K[i]->get_pointer();

            for(size_t n = 0; n < nbf2; n++)
                Kp[n] = my_K1[n] + my_K2[n];
        }
    }



#if 0
    // => Sizing <= //

    int nshell  = primary_->nshell();
    int nthread = df_ints_num_threads_;

    // => Task Blocking <= //

    std::vector<int> task_shells;
    std::vector<int> task_starts;

    // > Atomic Blocking < //

    int atomic_ind = -1;
    for (int P = 0; P < nshell; P++) {
        if (primary_->shell(P).ncenter() > atomic_ind) {
            task_starts.push_back(P);
            atomic_ind++;
        }
        task_shells.push_back(P);
    }
    task_starts.push_back(nshell);

    // < End Atomic Blocking > //

    size_t ntask = task_starts.size() - 1;

    std::vector<int> task_offsets;
    task_offsets.push_back(0);
    for (int P2 = 0; P2 < primary_->nshell(); P2++) {
        task_offsets.push_back(task_offsets[P2] + primary_->shell(task_shells[P2]).nfunction());
    }

    size_t max_task = 0L;
    for (size_t task = 0; task < ntask; task++) {
        size_t size = 0L;
        for (int P2 = task_starts[task]; P2 < task_starts[task+1]; P2++) {
            size += primary_->shell(task_shells[P2]).nfunction();
        }
        max_task = (max_task >= size ? max_task : size);
    }

    if (debug_) {
        outfile->Printf( "  ==> DirectJK: Task Blocking <==\n\n");
        for (size_t task = 0; task < ntask; task++) {
            outfile->Printf( "  Task: %3d, Task Start: %4d, Task End: %4d\n", task, task_starts[task], task_starts[task+1]);
            for (int P2 = task_starts[task]; P2 < task_starts[task+1]; P2++) {
                int P = task_shells[P2];
                int size = primary_->shell(P).nfunction();
                int off  = primary_->shell(P).function_index();
                int off2 = task_offsets[P2];
                outfile->Printf( "    Index %4d, Shell: %4d, Size: %4d, Offset: %4d, Offset2: %4d\n", P2, P, size, off, off2);
            }
        }
        outfile->Printf( "\n");
    }

    // => Significant Task Pairs (PQ|-style <= //

    std::vector<std::pair<int, int> > task_pairs;
    for (size_t Ptask = 0; Ptask < ntask; Ptask++) {
        for (size_t Qtask = 0; Qtask < ntask; Qtask++) {
            if (Qtask > Ptask) continue;
            bool found = false;
            for (int P2 = task_starts[Ptask]; P2 < task_starts[Ptask+1]; P2++) {
                for (int Q2 = task_starts[Qtask]; Q2 < task_starts[Qtask+1]; Q2++) {
                    int P = task_shells[P2];
                    int Q = task_shells[Q2];
                    if (sieve_->shell_pair_significant(P,Q)) {
                        found = true;
                        task_pairs.push_back(std::pair<int,int>(Ptask,Qtask));
                        break;
                    }
                }
                if (found) break;
            }
        }
    }
    size_t ntask_pair = task_pairs.size();
    size_t ntask_pair2 = ntask_pair * ntask_pair;

    // => Intermediate Buffers <= //

    std::vector<std::vector<std::shared_ptr<Matrix> > > JKT;
    for (int thread = 0; thread < nthread; thread++) {
        std::vector<std::shared_ptr<Matrix> > JK2;
        for (size_t ind = 0; ind < D.size(); ind++) {
            JK2.push_back(std::shared_ptr<Matrix>(new Matrix("JKT", (lr_symmetric_ ? 6 : 10) * max_task, max_task)));
        }
        JKT.push_back(JK2);
    }

    // => Benchmarks <= //

    size_t computed_shells = 0L;

    // ==> Master Task Loop <== //

    #pragma omp parallel for num_threads(nthread) schedule(dynamic) reduction(+: computed_shells)
    for (size_t task = 0L; task < ntask_pair2; task++) {

        size_t task1 = task / ntask_pair;
        size_t task2 = task % ntask_pair;

        int Ptask = task_pairs[task1].first;
        int Qtask = task_pairs[task1].second;
        int Rtask = task_pairs[task2].first;
        int Stask = task_pairs[task2].second;

        // GOTCHA! Thought this should be RStask > PQtask, but
        // H2/3-21G: Task (10|11) gives valid quartets (30|22) and (31|22)
        // This is an artifact that multiple shells on each task allow
        // for for the Ptask's index to possibly trump any RStask pair,
        // regardless of Qtask's index
        if (Rtask > Ptask) continue;

        //printf("Task: %2d %2d %2d %2d\n", Ptask, Qtask, Rtask, Stask);

        int nPtask = task_starts[Ptask + 1] - task_starts[Ptask];
        int nQtask = task_starts[Qtask + 1] - task_starts[Qtask];
        int nRtask = task_starts[Rtask + 1] - task_starts[Rtask];
        int nStask = task_starts[Stask + 1] - task_starts[Stask];

        int P2start = task_starts[Ptask];
        int Q2start = task_starts[Qtask];
        int R2start = task_starts[Rtask];
        int S2start = task_starts[Stask];

        int dPsize = task_offsets[P2start + nPtask] - task_offsets[P2start];
        int dQsize = task_offsets[Q2start + nQtask] - task_offsets[Q2start];
        int dRsize = task_offsets[R2start + nRtask] - task_offsets[R2start];
        int dSsize = task_offsets[S2start + nStask] - task_offsets[S2start];

        int thread = 0;
        #ifdef _OPENMP
            thread = omp_get_thread_num();
        #endif

        // => Master shell quartet loops <= //

        bool touched = false;
        for (int P2 = P2start; P2 < P2start + nPtask; P2++) {
        for (int Q2 = Q2start; Q2 < Q2start + nQtask; Q2++) {
            if (Q2 > P2) continue;
            int P = task_shells[P2];
            int Q = task_shells[Q2];
            if (!sieve_->shell_pair_significant(P,Q)) continue;
        for (int R2 = R2start; R2 < R2start + nRtask; R2++) {
        for (int S2 = S2start; S2 < S2start + nStask; S2++) {
            if (S2 > R2) continue;
            int R = task_shells[R2];
            int S = task_shells[S2];
            if (R2 * nshell + S2 > P2 * nshell + Q2) continue;
            if (!sieve_->shell_pair_significant(R,S)) continue;
            if (!sieve_->shell_significant(P,Q,R,S)) continue;

            //printf("Quartet: %2d %2d %2d %2d\n", P, Q, R, S);

            //if (thread == 0) timer_on("JK: Ints");
            if(ints[thread]->compute_shell(P,Q,R,S) == 0)
                continue; // No integrals in this shell quartet
            computed_shells++;
            //if (thread == 0) timer_off("JK: Ints");

            const double* buffer = ints[thread]->buffer();

            int Psize = primary_->shell(P).nfunction();
            int Qsize = primary_->shell(Q).nfunction();
            int Rsize = primary_->shell(R).nfunction();
            int Ssize = primary_->shell(S).nfunction();

            int Poff = primary_->shell(P).function_index();
            int Qoff = primary_->shell(Q).function_index();
            int Roff = primary_->shell(R).function_index();
            int Soff = primary_->shell(S).function_index();

            int Poff2 = task_offsets[P2] - task_offsets[P2start];
            int Qoff2 = task_offsets[Q2] - task_offsets[Q2start];
            int Roff2 = task_offsets[R2] - task_offsets[R2start];
            int Soff2 = task_offsets[S2] - task_offsets[S2start];

            //if (thread == 0) timer_on("JK: GEMV");
            for (size_t ind = 0; ind < D.size(); ind++) {
                double** Dp = D[ind]->pointer();
                double** JKTp = JKT[thread][ind]->pointer();
                const double* buffer2 = buffer;

                if (!touched) {
                    ::memset((void*) JKTp[0L * max_task], '\0', dPsize * dQsize * sizeof(double));
                    ::memset((void*) JKTp[1L * max_task], '\0', dRsize * dSsize * sizeof(double));
                    ::memset((void*) JKTp[2L * max_task], '\0', dPsize * dRsize * sizeof(double));
                    ::memset((void*) JKTp[3L * max_task], '\0', dPsize * dSsize * sizeof(double));
                    ::memset((void*) JKTp[4L * max_task], '\0', dQsize * dRsize * sizeof(double));
                    ::memset((void*) JKTp[5L * max_task], '\0', dQsize * dSsize * sizeof(double));
                    if (!lr_symmetric_) {
                        ::memset((void*) JKTp[6L * max_task], '\0', dRsize * dPsize * sizeof(double));
                        ::memset((void*) JKTp[7L * max_task], '\0', dSsize * dPsize * sizeof(double));
                        ::memset((void*) JKTp[8L * max_task], '\0', dRsize * dQsize * sizeof(double));
                        ::memset((void*) JKTp[9L * max_task], '\0', dSsize * dQsize * sizeof(double));
                    }
                }

                double* J1p = JKTp[0L * max_task];
                double* J2p = JKTp[1L * max_task];
                double* K1p = JKTp[2L * max_task];
                double* K2p = JKTp[3L * max_task];
                double* K3p = JKTp[4L * max_task];
                double* K4p = JKTp[5L * max_task];
                double* K5p;
                double* K6p;
                double* K7p;
                double* K8p;
                if (!lr_symmetric_) {
                    K5p = JKTp[6L * max_task];
                    K6p = JKTp[7L * max_task];
                    K7p = JKTp[8L * max_task];
                    K8p = JKTp[9L * max_task];
                }

                double prefactor = 1.0;
                if (P == Q)           prefactor *= 0.5;
                if (R == S)           prefactor *= 0.5;
                if (P == R && Q == S) prefactor *= 0.5;

                for (int p = 0; p < Psize; p++) {
                for (int q = 0; q < Qsize; q++) {
                for (int r = 0; r < Rsize; r++) {
                for (int s = 0; s < Ssize; s++) {
                    J1p[(p + Poff2) * dQsize + q + Qoff2] += prefactor * (Dp[r + Roff][s + Soff] + Dp[s + Soff][r + Roff]) * (*buffer2);
                    J2p[(r + Roff2) * dSsize + s + Soff2] += prefactor * (Dp[p + Poff][q + Qoff] + Dp[q + Qoff][p + Poff]) * (*buffer2);
                    K1p[(p + Poff2) * dRsize + r + Roff2] += prefactor * (Dp[q + Qoff][s + Soff]) * (*buffer2);
                    K2p[(p + Poff2) * dSsize + s + Soff2] += prefactor * (Dp[q + Qoff][r + Roff]) * (*buffer2);
                    K3p[(q + Qoff2) * dRsize + r + Roff2] += prefactor * (Dp[p + Poff][s + Soff]) * (*buffer2);
                    K4p[(q + Qoff2) * dSsize + s + Soff2] += prefactor * (Dp[p + Poff][r + Roff]) * (*buffer2);
                    if (!lr_symmetric_) {
                        K5p[(r + Roff2) * dPsize + p + Poff2] += prefactor * (Dp[s + Soff][q + Qoff]) * (*buffer2);
                        K6p[(s + Soff2) * dPsize + p + Poff2] += prefactor * (Dp[r + Roff][q + Qoff]) * (*buffer2);
                        K7p[(r + Roff2) * dQsize + q + Qoff2] += prefactor * (Dp[s + Soff][p + Poff]) * (*buffer2);
                        K8p[(s + Soff2) * dQsize + q + Qoff2] += prefactor * (Dp[r + Roff][p + Poff]) * (*buffer2);
                    }
                    buffer2++;
                }}}}

            }
            touched = true;
            //if (thread == 0) timer_off("JK: GEMV");

        }}}} // End Shell Quartets

        if (!touched) continue;

        // => Stripe out <= //

        //if (thread == 0) timer_on("JK: Atomic");
        for (size_t ind = 0; ind < D.size(); ind++) {
            double** JKTp = JKT[thread][ind]->pointer();
            double** Jp = J[ind]->pointer();
            double** Kp = K[ind]->pointer();

            double* J1p = JKTp[0L * max_task];
            double* J2p = JKTp[1L * max_task];
            double* K1p = JKTp[2L * max_task];
            double* K2p = JKTp[3L * max_task];
            double* K3p = JKTp[4L * max_task];
            double* K4p = JKTp[5L * max_task];
            double* K5p;
            double* K6p;
            double* K7p;
            double* K8p;
            if (!lr_symmetric_) {
                K5p = JKTp[6L * max_task];
                K6p = JKTp[7L * max_task];
                K7p = JKTp[8L * max_task];
                K8p = JKTp[9L * max_task];
            }

            // > J_PQ < //

            for (int P2 = 0; P2 < nPtask; P2++) {
            for (int Q2 = 0; Q2 < nQtask; Q2++) {
                int P = task_shells[P2start + P2];
                int Q = task_shells[Q2start + Q2];
                int Psize = primary_->shell(P).nfunction();
                int Qsize = primary_->shell(Q).nfunction();
                int Poff =  primary_->shell(P).function_index();
                int Qoff =  primary_->shell(Q).function_index();
                int Poff2 = task_offsets[P2 + P2start] - task_offsets[P2start];
                int Qoff2 = task_offsets[Q2 + Q2start] - task_offsets[Q2start];
                for (int p = 0; p < Psize; p++) {
                for (int q = 0; q < Qsize; q++) {
                    #pragma omp atomic
                    Jp[p + Poff][q + Qoff] += J1p[(p + Poff2) * dQsize + q + Qoff2];
                }}
            }}

            // > J_RS < //

            for (int R2 = 0; R2 < nRtask; R2++) {
            for (int S2 = 0; S2 < nStask; S2++) {
                int R = task_shells[R2start + R2];
                int S = task_shells[S2start + S2];
                int Rsize = primary_->shell(R).nfunction();
                int Ssize = primary_->shell(S).nfunction();
                int Roff =  primary_->shell(R).function_index();
                int Soff =  primary_->shell(S).function_index();
                int Roff2 = task_offsets[R2 + R2start] - task_offsets[R2start];
                int Soff2 = task_offsets[S2 + S2start] - task_offsets[S2start];
                for (int r = 0; r < Rsize; r++) {
                for (int s = 0; s < Ssize; s++) {
                    #pragma omp atomic
                    Jp[r + Roff][s + Soff] += J2p[(r + Roff2) * dSsize + s + Soff2];
                }}
            }}

            // > K_PR < //

            for (int P2 = 0; P2 < nPtask; P2++) {
            for (int R2 = 0; R2 < nRtask; R2++) {
                int P = task_shells[P2start + P2];
                int R = task_shells[R2start + R2];
                int Psize = primary_->shell(P).nfunction();
                int Rsize = primary_->shell(R).nfunction();
                int Poff =  primary_->shell(P).function_index();
                int Roff =  primary_->shell(R).function_index();
                int Poff2 = task_offsets[P2 + P2start] - task_offsets[P2start];
                int Roff2 = task_offsets[R2 + R2start] - task_offsets[R2start];
                for (int p = 0; p < Psize; p++) {
                for (int r = 0; r < Rsize; r++) {
                    #pragma omp atomic
                    Kp[p + Poff][r + Roff] += K1p[(p + Poff2) * dRsize + r + Roff2];
                    if (!lr_symmetric_) {
                        #pragma omp atomic
                        Kp[r + Roff][p + Poff] += K5p[(r + Roff2) * dPsize + p + Poff2];
                    }
                }}
            }}

            // > K_PS < //

            for (int P2 = 0; P2 < nPtask; P2++) {
            for (int S2 = 0; S2 < nStask; S2++) {
                int P = task_shells[P2start + P2];
                int S = task_shells[S2start + S2];
                int Psize = primary_->shell(P).nfunction();
                int Ssize = primary_->shell(S).nfunction();
                int Poff =  primary_->shell(P).function_index();
                int Soff =  primary_->shell(S).function_index();
                int Poff2 = task_offsets[P2 + P2start] - task_offsets[P2start];
                int Soff2 = task_offsets[S2 + S2start] - task_offsets[S2start];
                for (int p = 0; p < Psize; p++) {
                for (int s = 0; s < Ssize; s++) {
                    #pragma omp atomic
                    Kp[p + Poff][s + Soff] += K2p[(p + Poff2) * dSsize + s + Soff2];
                    if (!lr_symmetric_) {
                        #pragma omp atomic
                        Kp[s + Soff][p + Poff] += K6p[(s + Soff2) * dPsize + p + Poff2];
                    }
                }}
            }}

            // > K_QR < //

            for (int Q2 = 0; Q2 < nQtask; Q2++) {
            for (int R2 = 0; R2 < nRtask; R2++) {
                int Q = task_shells[Q2start + Q2];
                int R = task_shells[R2start + R2];
                int Qsize = primary_->shell(Q).nfunction();
                int Rsize = primary_->shell(R).nfunction();
                int Qoff =  primary_->shell(Q).function_index();
                int Roff =  primary_->shell(R).function_index();
                int Qoff2 = task_offsets[Q2 + Q2start] - task_offsets[Q2start];
                int Roff2 = task_offsets[R2 + R2start] - task_offsets[R2start];
                for (int q = 0; q < Qsize; q++) {
                for (int r = 0; r < Rsize; r++) {
                    #pragma omp atomic
                    Kp[q + Qoff][r + Roff] += K3p[(q + Qoff2) * dRsize + r + Roff2];
                    if (!lr_symmetric_) {
                        #pragma omp atomic
                        Kp[r + Roff][q + Qoff] += K7p[(r + Roff2) * dQsize + q + Qoff2];
                    }
                }}
            }}

            // > K_QS < //

            for (int Q2 = 0; Q2 < nQtask; Q2++) {
            for (int S2 = 0; S2 < nStask; S2++) {
                int Q = task_shells[Q2start + Q2];
                int S = task_shells[S2start + S2];
                int Qsize = primary_->shell(Q).nfunction();
                int Ssize = primary_->shell(S).nfunction();
                int Qoff =  primary_->shell(Q).function_index();
                int Soff =  primary_->shell(S).function_index();
                int Qoff2 = task_offsets[Q2 + Q2start] - task_offsets[Q2start];
                int Soff2 = task_offsets[S2 + S2start] - task_offsets[S2start];
                for (int q = 0; q < Qsize; q++) {
                for (int s = 0; s < Ssize; s++) {
                    #pragma omp atomic
                    Kp[q + Qoff][s + Soff] += K4p[(q + Qoff2) * dSsize + s + Soff2];
                    if (!lr_symmetric_) {
                        #pragma omp atomic
                        Kp[s + Soff][q + Qoff] += K8p[(s + Soff2) * dQsize + q + Qoff2];
                    }
                }}
            }}

        } // End stripe out
        //if (thread == 0) timer_off("JK: Atomic");

    } // End master task list

    for (size_t ind = 0; ind < D.size(); ind++) {
        J[ind]->scale(2.0);
        J[ind]->hermitivitize();
        if (lr_symmetric_) {
            K[ind]->scale(2.0);
            K[ind]->hermitivitize();
        }
    }

    if (bench_) {
       std::shared_ptr<OutFile> printer(new OutFile("bench.dat",APPEND));
        size_t ntri = nshell * (nshell + 1L) / 2L;
        size_t possible_shells = ntri * (ntri + 1L) / 2L;
        printer->Printf( "Computed %20zu Shell Quartets out of %20zu, (%11.3E ratio)\n", computed_shells, possible_shells, computed_shells / (double) possible_shells);
    }
#endif
}

#if 0



DirectJK::DirectJK(std::shared_ptr<BasisSet> primary) :
   JK(primary)
{
    common_init();
}
DirectJK::~DirectJK()
{
}
void DirectJK::common_init()
{
}
void DirectJK::print_header() const
{
    if (print_) {
        outfile->Printf( "  ==> DirectJK: Integral-Direct J/K Matrices <==\n\n");

        outfile->Printf( "    J tasked:          %11s\n", (do_J_ ? "Yes" : "No"));
        outfile->Printf( "    K tasked:          %11s\n", (do_K_ ? "Yes" : "No"));
        outfile->Printf( "    wK tasked:         %11s\n", (do_wK_ ? "Yes" : "No"));
        if (do_wK_)
            outfile->Printf( "    Omega:             %11.3E\n", omega_);
        outfile->Printf( "    OpenMP threads:    %11d\n", omp_nthread_);
        outfile->Printf( "    Memory (MB):       %11ld\n", (memory_ *8L) / (1024L * 1024L));
        outfile->Printf( "    Schwarz Cutoff:    %11.0E\n\n", cutoff_);
    }
}
void DirectJK::preiterations()
{
    sieve_ = std::shared_ptr<ERISieve>(new ERISieve(primary_, cutoff_));
    factory_= std::shared_ptr<IntegralFactory>(new IntegralFactory(primary_,primary_,primary_,primary_));
    eri_.clear();
    for (int thread = 0; thread < omp_nthread_; thread++) {
        eri_.push_back(std::shared_ptr<TwoBodyAOInt>(factory_->erd_eri()));
    }
}
void DirectJK::compute_JK()
{
    // Correctness always counts
    const double* buffer = eri_[0]->buffer();
    for (int M = 0; M < primary_->nshell(); ++M) {
    for (int N = 0; N < primary_->nshell(); ++N) {
    for (int R = 0; R < primary_->nshell(); ++R) {
    for (int S = 0; S < primary_->nshell(); ++S) {

        if(eri_[0]->compute_shell(M,N,R,S) == 0)
            continue; // No integrals were computed here

        int nM = primary_->shell(M).nfunction();
        int nN = primary_->shell(N).nfunction();
        int nR = primary_->shell(R).nfunction();
        int nS = primary_->shell(S).nfunction();

        int sM = primary_->shell(M).function_index();
        int sN = primary_->shell(N).function_index();
        int sR = primary_->shell(R).function_index();
        int sS = primary_->shell(S).function_index();

        for (int oM = 0, index = 0; oM < nM; oM++) {
        for (int oN = 0; oN < nN; oN++) {
        for (int oR = 0; oR < nR; oR++) {
        for (int oS = 0; oS < nS; oS++, index++) {

            double val = buffer[index];

            int m = oM + sM;
            int n = oN + sN;
            int r = oR + sR;
            int s = oS + sS;

            if (do_J_) {
                for (int N = 0; N < J_ao_.size(); N++) {
                    J_ao_[N]->add(0,m,n, D_ao_[N]->get(0,r,s)*val);
                }
            }

            if (do_K_) {
                for (int N = 0; N < K_ao_.size(); N++) {
                    K_ao_[N]->add(0,m,s, D_ao_[N]->get(0,n,r)*val);
                }
            }

        }}}}

    }}}}

    // Faster version, not finished
    /**
    sieve_->set_sieve(cutoff_);
    const std::vector<std::pair<int,int> >& shell_pairs = sieve_->shell_pairs();
    unsigned long int nMN = shell_pairs.size();
    unsigned long int nMNRS = nMN * nMN;
    int nthread = eri_.size();

    #pragma omp parallel for schedule(dynamic,30) num_threads(nthread)
    for (unsigned long int index = 0L; index < nMNRS; ++index) {

        int thread = 0;
        #ifdef _OPENMP
            thread = omp_get_thread_num();
        #endif

        const double* buffer = eri_[thread]->buffer();

        unsigned long int MN = index / nMN;
        unsigned long int RS = index % nMN;
        if (MN < RS) continue;

        int M = shell_pairs[MN].first;
        int N = shell_pairs[MN].second;
        int R = shell_pairs[RS].first;
        int S = shell_pairs[RS].second;

        eri_[thread]->compute_shell(M,N,R,S);

        int nM = primary_->shell(M)->nfunction();
        int nN = primary_->shell(N)->nfunction();
        int nR = primary_->shell(R)->nfunction();
        int nS = primary_->shell(S)->nfunction();

        int sM = primary_->shell(M)->function_index();
        int sN = primary_->shell(N)->function_index();
        int sR = primary_->shell(R)->function_index();
        int sS = primary_->shell(S)->function_index();

        for (int oM = 0, index = 0; oM < nM; oM++) {
        for (int oN = 0; oN < nN; oN++) {
        for (int oR = 0; oR < nR; oR++) {
        for (int oS = 0; oS < nS; oS++, index++) {

            int m = oM + sM;
            int n = oN + sN;
            int r = oR + sR;
            int s = oS + sS;

            if ((n > m) || (s > r) || ((r*(r+1) >> 1) + s > (m*(m+1) >> 1) + n)) continue;

            double val = buffer[index];

            if (do_J_) {
                for (int N = 0; N < J_ao_.size(); N++) {
                    double** Dp = D_ao_[N]->pointer();
                    double** Jp = J_ao_[N]->pointer();

                    // I've given you all the unique ones
                    // Make sure to use #pragma omp atomic
                    // TODO
                }
            }

            if (do_K_) {
                for (int N = 0; N < K_ao_.size(); N++) {
                    double** Dp = D_ao_[N]->pointer();
                    double** Kp = J_ao_[N]->pointer();

                    // I've given you all the unique ones
                    // Make sure to use #pragma omp atomic
                    // TODO
                }
            }

        }}}}

    }
    **/
}

#endif


}
