//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2018 QMCPACK developers.
//
// File developed by: Ye Luo, yeluo@anl.gov, Argonne National Laboratory
//
// File created by: Ye Luo, yeluo@anl.gov, Argonne National Laboratory
//////////////////////////////////////////////////////////////////////////////////////

#ifndef QMCPLUSPLUS_DELAYED_UPDATE_CUDA_H
#define QMCPLUSPLUS_DELAYED_UPDATE_CUDA_H

#include "Numerics/Blasf.h"
#include <OhmmsPETE/OhmmsVector.h>
#include <OhmmsPETE/OhmmsMatrix.h>
#include "QMCWaveFunctions/Fermion/DiracMatrix.h"
#include <simd/simd.hpp>
#include "simd/CUDAallocator.hpp"
#include <cublas_v2.h>
#include "QMCWaveFunctions/Fermion/delayed_update_helper.h"

namespace qmcplusplus {

  template<typename T, typename T_hp>
    struct DelayedUpdateCUDA
    {
      Matrix<T> U, V, B, Binv;
      //Matrix<T> tempMat; // for debugging only
      Matrix<T, CUDAAllocator<T>, false> U_gpu, V_gpu, Binv_gpu, temp_gpu, Ainv_gpu;
      Vector<T> temp, rcopy;
      Matrix<T_hp> Binv_hp;
      DiracMatrix<T_hp> deteng;
      std::vector<int> delay_list;
      Vector<int, CUDAAllocator<int>, false> delay_list_gpu;
      int delay_count;

      const T* Ainv_row_ptr;
      T curRatio;

      // CUDA specific variables
      cublasHandle_t handle;
      cudaStream_t hstream;

      DelayedUpdateCUDA(): delay_count(0), Ainv_row_ptr(nullptr)
      {
        cublasCreate(&handle);
        cudaStreamCreate(&hstream);
        cublasSetStream(handle,hstream);
      }

      ~DelayedUpdateCUDA()
      {
        cublasDestroy(handle);
        cudaStreamDestroy(hstream);
      }

      inline void resize(int norb, int delay)
      {
        //tempMat.resize(norb, delay);
        V.resize(delay, norb);
        U.resize(delay, norb);
        B.resize(delay, delay);
        Binv.resize(delay, delay);
#ifdef MIXED_PRECISION
        Binv_hp.resize(delay, delay);
        deteng.reset(Binv_hp, delay);
#else
        deteng.reset(Binv, delay);
#endif
        delay_count = 0;

        temp_gpu.resize(norb, delay);
        delay_list.resize(delay);
        U_gpu.resize(delay, norb);
        V_gpu.resize(delay, norb);
        Binv_gpu.resize(delay, delay);
        Ainv_gpu.resize(norb, norb);
        delay_list_gpu.resize(delay);
      }

      inline void transferAinvH2D(const Matrix<T>& Ainv)
      {
        cudaMemcpyAsync(Ainv_gpu.data(), Ainv.data(), Ainv.size()*sizeof(T), cudaMemcpyHostToDevice, hstream);
      }

      inline void getInvRow(const Matrix<T>& Ainv, int rowchanged)
      {
        if ( delay_count == 0 )
        {
          Ainv_row_ptr = Ainv[rowchanged];
          return;
        }
        CONSTEXPR T cone(1);
        CONSTEXPR T czero(0);
        const T* AinvRow = Ainv[rowchanged];
        const int norb = Ainv.rows();
        const int lda_Binv = Binv.cols();
        T temp[lda_Binv];
        // save AinvRow to new_AinvRow
        simd::copy_n(AinvRow, norb, V[delay_count]);
        // multiply V (NxK) Binv(KxK) U(KxN) AinvRow right to the left
        BLAS::gemv('T', norb, delay_count, cone, U.data(), norb, AinvRow, 1, czero, B[delay_count], 1);
        BLAS::gemv('N', delay_count, delay_count, cone, Binv.data(), lda_Binv, B[delay_count], 1, czero, temp, 1);
        BLAS::gemv('N', norb, delay_count, -cone, V.data(), norb, temp, 1, cone, V[delay_count], 1);
        Ainv_row_ptr = V[delay_count];
      }

      template<typename VVT>
      inline T ratio(const Matrix<T>& Ainv, int rowchanged, const VVT& psiV)
      {
        getInvRow(Ainv, rowchanged);
        return curRatio = simd::dot(Ainv_row_ptr,psiV.data(),Ainv.cols());
      }

      template<typename GT>
      inline GT evalGrad(const Matrix<T>& Ainv, int rowchanged, const GT* dpsiV)
      {
        getInvRow(Ainv, rowchanged);
        return simd::dot(Ainv_row_ptr,dpsiV,Ainv.cols());
      }

      template<typename VVT, typename GGT, typename GT>
      inline T ratioGrad(const Matrix<T>& Ainv, int rowchanged, const VVT& psiV, const GGT& dpsiV, GT& g)
      {
        if( delay_count == 0 )
        {
          g = simd::dot(Ainv[rowchanged],dpsiV.data(),Ainv.cols());
          return curRatio = simd::dot(Ainv[rowchanged],psiV.data(),Ainv.cols());
        }
        else if( Ainv_row_ptr )
        {
          g = simd::dot(Ainv_row_ptr,dpsiV.data(),Ainv.cols());
          return curRatio = simd::dot(Ainv_row_ptr,psiV.data(),Ainv.cols());
        }
        else
        {
          throw std::runtime_error("DelayedUpdateCUDA : this should never happen!\n");
          return T(0);
        }
      }

      // SM-1 Fahy immediate update
      template<typename VVT>
      inline void updateRow(Matrix<T>& a, int rowchanged, const VVT& psiV)
      {
        // safe mechanism
        Ainv_row_ptr = nullptr;

        const int m = a.rows();
        const int lda = a.cols();
        CONSTEXPR T cone(1);
        CONSTEXPR T czero(0);
        temp.resize(lda);
        rcopy.resize(lda);
        T c_ratio = cone / curRatio;
        BLAS::gemv('T', m, m, c_ratio, a.data(), lda, psiV.data(), 1, czero, temp.data(), 1);
        temp[rowchanged] = cone-c_ratio;
        simd::copy_n(a[rowchanged],m,rcopy.data());
        BLAS::ger(m,m,-cone,rcopy.data(),1,temp.data(),1,a.data(),lda);
      }

      // accept with the update delayed
      template<typename VVT>
      inline void acceptRow(Matrix<T>& Ainv, int rowchanged, const VVT& psiV)
      {
        // safe mechanism
        Ainv_row_ptr = nullptr;

        CONSTEXPR T cone(1);
        CONSTEXPR T czero(0);
        const int norb = Ainv.rows();
        const int lda_Binv = Binv.cols();
        simd::copy_n(Ainv[rowchanged], norb, V[delay_count]);
        simd::copy_n(psiV.data(), norb, U[delay_count]);
        delay_list[delay_count] = rowchanged;
        delay_count++;
        // the new row in B has been computed, now compute the new column
        if(delay_count==1)
        {
          B[0][0] = curRatio;
          Binv[0][0] = T_hp(1.0) / static_cast<T_hp>(B[0][0]);
        }
        else
        {
          BLAS::gemv('T', norb, delay_count, cone, V.data(), norb, psiV.data(), 1, czero, B.data()+delay_count-1, lda_Binv);
#ifdef MIXED_PRECISION
          for(int i=0; i<delay_count; i++)
            for(int j=0; j<delay_count; j++)
              Binv_hp[i][j] = B[i][j];
          deteng.invert(Binv_hp,false,delay_count);
          for(int i=0; i<delay_count; i++)
            for(int j=0; j<delay_count; j++)
              Binv[i][j] = Binv_hp[i][j];
#else
          for(int i=0; i<delay_count; i++)
            for(int j=0; j<delay_count; j++)
              Binv[i][j] = B[i][j];
          deteng.invert(Binv,false,delay_count);
#endif
        }
        if(delay_count==lda_Binv) updateInvMat(Ainv);
      }

      inline void updateInvMat(Matrix<T>& Ainv, bool wait_async = true)
      {
        //if(delay_count==1)
        //{
        //  temp.resize(norb);
        //  // Only use the first norb elements of temp_gpu as a temporal array
        //  BLAS::gemv('T', norb, norb, cone, Ainv.data(), norb, U[0], 1, czero, temp.data(), 1);
        //  temp[delay_list[0]] -= cone;
        //  BLAS::ger(norb,norb,-Binv[0][0],V[0],1,temp.data(),1,Ainv.data(),norb);
        //}
        //else
        // update the inverse matrix
        if( delay_count>0 )
        {
          CONSTEXPR T cone(1);
          CONSTEXPR T czero(0);
          const int norb=Ainv.rows();
          const int lda_Binv=Binv.cols();
          const T cminusone(-1);
              cudaError_t error(cudaSuccess);
              cublasStatus_t cublas_error;
              int deviceid;
              cudaGetDevice(&deviceid);
              //std::cout << "delay_count = " << delay_count << " id = " << deviceid << std::endl;
          error = cudaMemcpyAsync(U_gpu.data(), U.data(), norb*delay_count*sizeof(T), cudaMemcpyHostToDevice, hstream);
              if( error!=cudaSuccess ) std::cout <<"debug error 1 code " << error << std::endl;
          //BLAS::gemm('T', 'N', delay_count, norb, norb, cone, U.data(), norb, Ainv.data(), norb, czero, tempMat.data(), lda_Binv);
          //    cudaMemPrefetchAsync(Ainv.data(), Ainv.size()*sizeof(T), 0, hstream);
          cublas_error = cublasDgemm(handle, CUBLAS_OP_T, CUBLAS_OP_N, delay_count, norb, norb, &cone, U_gpu.data(), norb, Ainv_gpu.data(), norb, &czero, temp_gpu.data(), lda_Binv);
              if( cublas_error!=CUBLAS_STATUS_SUCCESS ) std::cout <<"debug cublas error 1 code " << cublas_error << std::endl;
          error = cudaMemcpyAsync(delay_list_gpu.data(), delay_list.data(), delay_count*sizeof(int), cudaMemcpyHostToDevice, hstream);
              if( error!=cudaSuccess ) std::cout <<"debug error 2 code " << error << std::endl;
          //for(int i=0; i<delay_count; i++) tempMat(delay_list[i], i) -= cone;
          applyW_stageV_cuda(delay_list_gpu.data(), delay_count, temp_gpu.data(), norb, temp_gpu.cols(), V_gpu.data(), Ainv_gpu.data(), hstream);
          //error = cudaMemcpyAsync(tempMat.data(), temp_gpu.data(), tempMat.size()*sizeof(T), cudaMemcpyDeviceToHost, hstream);
          //    if( error!=cudaSuccess ) std::cout <<"debug error 3 code " << error << std::endl;
          //error = cudaStreamSynchronize(hstream);
          //    if( error!=cudaSuccess ) std::cout <<"debug error 4 code " << error << std::endl;
              //std::cout << "debug tempMat " << tempMat << std::endl;
          cudaMemcpyAsync(Binv_gpu.data(), Binv.data(), lda_Binv*delay_count*sizeof(T), cudaMemcpyHostToDevice, hstream);
          //BLAS::gemm('N', 'N', norb, delay_count, delay_count, cone, V.data(), norb, Binv.data(), lda_Binv, czero, U.data(), norb);
          cublasDgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N, norb, delay_count, delay_count, &cone, V_gpu.data(), norb, Binv_gpu.data(), lda_Binv, &czero, U_gpu.data(), norb);
          //error = cudaMemcpyAsync(U.data(), U_gpu.data(), norb*delay_count*sizeof(T), cudaMemcpyDeviceToHost, hstream);
          //    if( error!=cudaSuccess ) std::cout <<"debug error 5 code " << error << std::endl;
          //error = cudaStreamSynchronize(hstream);
          //    if( error!=cudaSuccess ) std::cout <<"debug error 6 code " << error << std::endl;
              //std::cout << "debug U " << U << std::endl;
          //BLAS::gemm('N', 'N', norb, norb, delay_count, -cone, U.data(), norb, tempMat.data(), lda_Binv, cone, Ainv.data(), norb);
          cublasDgemm(handle, CUBLAS_OP_N, CUBLAS_OP_N, norb, norb, delay_count, &cminusone, U_gpu.data(), norb, temp_gpu.data(), lda_Binv, &cone, Ainv_gpu.data(), norb);
          error = cudaMemcpyAsync(Ainv.data(), Ainv_gpu.data(), norb*norb*sizeof(T), cudaMemcpyDeviceToHost, hstream);
        }
        delay_count = 0;

        // block incomplete stream execution
        if(wait_async) waitStream();
      }

      inline void waitStream()
      {
        cudaError_t error = cudaStreamSynchronize(hstream);
        if( error!=cudaSuccess ) throw std::runtime_error("DelayedUpdateCUDA : cudaStreamSynchronize failed!\n");
      }
    };
}

#endif // QMCPLUSPLUS_DELAYED_UPDATE_H

