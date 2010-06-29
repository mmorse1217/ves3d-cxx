void DotProductGpu(const float *a, const float *b, int stride, int num_surfs, float *aDb);
void CrossProductGpu(const float *a, const float *b, int stride, int num_surfs, float *aCb);
void xvpwGpu(const float *x, const float *a, const float *y, int stride, int num_surfs, float *AxPy);
void xvpbGpu(const float *x, const float *a, float y, int stride, int num_surfs, float *AxPy);
void uyInvGpu(const float *x, const float *y, int stride, int num_surfs, float *xDy);
void SqrtGpu(const float* x_in, int length, float *x_out);
void InvGpu(const float* x_in, int length, float *x_out);
void xyGpu(const float* x_in, const float *y_in, int length, float *xy_out);
void xyInvGpu(const float* x_in, const float *y_in, int length, float *xDy_out);
void ReduceGpu(const float *x_in, const float *w_in, const float *q_in, int stride, int num_surfs, float *int_x_dw);
void CircShiftGpu(const float *arr_in, int n_vecs, int vec_length, int shift, float *arr_out);
void axpyGpu(float a, const float* x_in, const float *y_in, int length, float *axpy_out);
void axpbGpu(float a, const float* x_in, float b, int length, float *axpb_out);
void cuda_shuffle(float *in, int stride, int n_surfs, int dim, float *out);
void cuda_stokes(int m, int n, int t_head, int t_tail, const float *T, const float *S, const float *D, float *U, const float *Q);
void ResampleGpu(int p, int n_funs, int q, const float *shc_p, float *shc_q);
void ScaleFreqsGpu(int p, int n_funs, const float *shc_in, const float *alpha, float *shc_out);
void avpwGpu(const float *a_in, const float *v_in, const float *w_in, int stride, int num_surfs, float *avpw_out);
float maxGpu(float *in, int n);

#include "transpose_kernel.h"
