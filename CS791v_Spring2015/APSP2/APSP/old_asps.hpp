/**
 *  All-pairs-shortest paths
 *	Device code (column-wise)
 *  Recursive in-place implementation 
 *  Copyright by Aydin Buluc
 *  June 2008
 */

#ifndef _APSP_KERNEL_H_
#define _APSP_KERNEL_H_

#include "cuda_runtime.h"
#include "device_launch_parameters.h"

#include <common_functions.h>

#include <stdio.h>

#include <float.h>


// IMPORTANT !!! NEVER MAKE BLOCK_SIZE > 16, more than 512 threads are not allowed !!! 
// It won't give you a warning, but run incorrectly

//#define VERIFY
#define FLOATINF FLT_MAX

// Parameters for simple GEMM kernel
#define BLOCK_SIZE 32//16	  	// each block is of size (BLOCK_SIZE x BLOCK_SIZE)
#define FAST_GEMM 64		// for matrices of dimensions > (FAST_GEMM x FAST_GEMM), execute Volkov's code

// Parameters for Volkov's kernel
#define UNROLL 4		// inner blocking dimension
#define ABLOCK 16
#define BBLOCK 64   		// multiplications are between (ABLOCK x UNROLL for A) and (UNROLL  x BBLOCK for B)

#define WA 1024			// matrix dimension (problem size), should be = BLOCK_SIZE * BLOCK_DIM 


#define CHECK_BANK_CONFLICTS 0
#if CHECK_BANK_CONFLICTS
#define AS(i, j) CUT_BANK_CHECKER(((float*)&As[0][0]), (BLOCK_SIZE * i + j))
#define BS(i, j) CUT_BANK_CHECKER(((float*)&Bs[0][0]), (BLOCK_SIZE * i + j))
#else
#define AS(i, j) As[i][j]
#define BS(i, j) Bs[i][j]
#endif



/**
 * APSP using a single block (column version)
 * Iteration is within this kernel function, 
 * So, no looping is necessary when calling apsp_seq
 * start is the starting offset of nboth dimensions
 */
__global__
void
apsp_seq(float * A, int multiplicand_width, int start) {
  // Thread index
  int t_x = threadIdx.x;
  int t_y = threadIdx.y;

  // Csub is used to store the element of the result
  // that is computed by the thread

  unsigned offset = start * WA + start;	// memory offset
  unsigned result_idx = offset + t_y * WA + t_x;
  for (int k = 0; k < multiplicand_width; ++k) {
    float M1 = A[offset + t_y * WA + k  ];		// kth row
    float M2 = A[offset + k   * WA + t_x];		// kth column
  
    A[result_idx] = fminf(M1 + M2, A[result_idx]);

    __syncthreads();
  }
}

template <int D>
__device__ void saxpy_MinPlus(float a, float *b, float *c) {
  for(int i=0; i<D; i++) {
    c[i] = fminf(a+b[i], c[i]);
  }
}


/**
 * Matrix multiplication on the device: C = A * B (column-major)
 * wA is A's and B's width
 * Each block uses shared memory of (nIt * 2 * 16 * 16 * 4) = 2048 bytes (BLOCK_SIZE=16, sizeof(WORD)=4)
 * nIt is at most BLOCK_DIM/2 but does not affect the amount of shared memory used 
 * each multiprocessor can execute at most 8 blocks simultaneously (due to shared memory constraints) 
 */
__global__ void
matrixMul( float * C, float * A, float * B, int wA, int sCx, int sCy, int sAx, int sAy, int sBx, int sBy, int add)
{
    // Block index
    int bx = blockIdx.x;
    int by = blockIdx.y;

    // Thread index
    int tx = threadIdx.x;
    int ty = threadIdx.y;

  // Remember... column-major
    int sa = sAx * WA + sAy;
    int sb = sBx * WA + sBy;
  int sc = sCx * WA + sCy;
    
    int ba = BLOCK_SIZE * by;			// y-offset
    int bb = WA * BLOCK_SIZE * bx;		// x-offset
    
    float min = FLOATINF;
    
    int nIt = wA / BLOCK_SIZE;	// number of blocks in one dimension
    
    // Do block multiplication to update the C(i,j) block
    // Using A(i,1) * A(1,j) + A(i,2) * A(2,j) + ... + A(i,n) * A(n,j)
    for(int m = 0; m < nIt; ++m)
    {
        __shared__ float As[BLOCK_SIZE * BLOCK_SIZE];
        __shared__ float Bs[BLOCK_SIZE * BLOCK_SIZE];
    
        //load one element each
        As[tx*BLOCK_SIZE + ty] = A[sa + ba + m * BLOCK_SIZE * WA + tx *WA + ty];
        Bs[tx*BLOCK_SIZE + ty] = B[sb + bb + m * BLOCK_SIZE  + tx *WA + ty];
        __syncthreads();
    
    
        for(int k = 0; k < BLOCK_SIZE; ++k)
        {
            float a = As[k * BLOCK_SIZE + tx];	// (tx)th row
            float b = Bs[ty * BLOCK_SIZE + k];	// (ty)th column

        min = fminf(a+b, min);
    }
        __syncthreads();    
    }
    // Write the block sub-matrix to device memory;
    // each thread writes one element

  if(add)
    C[sc + ba + bb + ty * WA + tx] = fminf(C[sc + ba + bb + ty * WA + tx], min);	
  else
    C[sc + ba + bb + ty * WA + tx] = min;		// (tx,ty)th element
}




__global__ void sgemmNN_MinPlus(
  const float *A, int lda, const float *B, int ldb, float* C, int ldc, int k, float beta )
{
  const int inx = threadIdx.x;
  const int iny = threadIdx.y;
  const int ibx = blockIdx.x * 64;
  const int iby = blockIdx.y * 16;
  const int id = inx + iny*16;
  
  A += ibx + id;
  B += inx + ((iby + iny) * ldb);  //__mul24( iby + iny, ldb );
  C += ibx + id  + (iby * ldc); //__mul24( iby, ldc );
  
  const float *Blast = B + k;
  

  float c[16] = {
    FLOATINF, FLOATINF, FLOATINF, FLOATINF,
    FLOATINF, FLOATINF, FLOATINF, FLOATINF,
    FLOATINF, FLOATINF, FLOATINF, FLOATINF,
    FLOATINF, FLOATINF, FLOATINF, FLOATINF
  };
    
  
  do {
    float a[4] = { A[0*lda], A[1*lda], A[2*lda], A[3*lda] };

    __shared__ float bs[16][17];
    bs[inx][iny]    = B[0*ldb];
    bs[inx][iny+4]  = B[4*ldb];
    bs[inx][iny+8]  = B[8*ldb];
    bs[inx][iny+12] = B[12*ldb];
    __syncthreads();

    A += 4*lda;
    saxpy_MinPlus<16>( a[0], &bs[0][0], c );		a[0] = A[0*lda];
    saxpy_MinPlus<16>( a[1], &bs[1][0], c );		a[1] = A[1*lda];
    saxpy_MinPlus<16>( a[2], &bs[2][0], c );		a[2] = A[2*lda];
    saxpy_MinPlus<16>( a[3], &bs[3][0], c );		a[3] = A[3*lda];	

    A += 4*lda;
    saxpy_MinPlus<16>( a[0], &bs[4][0], c );		a[0] = A[0*lda];
    saxpy_MinPlus<16>( a[1], &bs[5][0], c );		a[1] = A[1*lda];
    saxpy_MinPlus<16>( a[2], &bs[6][0], c );		a[2] = A[2*lda];
    saxpy_MinPlus<16>( a[3], &bs[7][0], c );		a[3] = A[3*lda];
    
    A += 4*lda;
    saxpy_MinPlus<16>( a[0], &bs[8][0], c );		a[0] = A[0*lda];
    saxpy_MinPlus<16>( a[1], &bs[9][0], c );		a[1] = A[1*lda];
    saxpy_MinPlus<16>( a[2], &bs[10][0], c );		a[2] = A[2*lda];
    saxpy_MinPlus<16>( a[3], &bs[11][0], c );		a[3] = A[3*lda];
    
    A += 4*lda;
    saxpy_MinPlus<16>( a[0], &bs[12][0], c );
    saxpy_MinPlus<16>( a[1], &bs[13][0], c );
    saxpy_MinPlus<16>( a[2], &bs[14][0], c );
    saxpy_MinPlus<16>( a[3], &bs[15][0], c );
    
    B += 16;
    __syncthreads();
  } while( B < Blast );

  for( int i = 0; i < 16; ++i, C += ldc ) {
    C[0] = fminf(c[i],beta*C[0]);
  }    
}	

#endif // #ifndef _APSP_KERNEL_H_