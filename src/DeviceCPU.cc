/**
 * @file   DeviceCPU.cc
 * @author Abtin Rahimian <arahimian@acm.org>
 * @date   Tue Feb 23 15:28:14 2010
 * 
 * @brief  The implementation of the Device class.
 */

inline pair<int, int> gridDimOf(int sh_order)
{
    return(make_pair(sh_order + 1, 2 * sh_order));
}

template<enum DeviceType DT>
Device<DT>::Device(int device_id, enum DeviceError *err)
{
    if(err!=0) *err = Success;
}

template<enum DeviceType DT>
Device<DT>::~Device()
{}

template<>
void* Device<CPU>::Malloc(size_t length) const
{
    PROFILESTART();
    void* ptr = ::malloc(length);
    PROFILEEND("CPU",0);
    return(ptr);
}

template<>
void Device<CPU>::Free(void* ptr) const
{
    PROFILESTART();
    ::free(ptr);
    ptr = 0;
    PROFILEEND("CPU",0);
}

template<>
void* Device<CPU>::Calloc(size_t num, size_t size) const
{
    PROFILESTART();
    void * ptr = ::calloc(num, size);
    PROFILEEND("CPU",0);
    return(ptr);
}

template<>
void* Device<CPU>::Memcpy(void* destination, const void* source, 
    size_t num, enum MemcpyKind kind) const
{
    PROFILESTART();
    ::memcpy(destination, source, num);
    PROFILEEND("CPU",0);
    return(destination);
}

template<>
void* Device<CPU>::Memset(void *ptr, int value, size_t num) const
{
    PROFILESTART();
    ::memset(ptr, value, num);
    PROFILEEND("CPU",0);
    return(ptr);
}

template<>
template<typename T>  
T* Device<CPU>::DotProduct(const T* u_in, const T* v_in, size_t stride, 
    size_t n_vecs, T* x_out) const
{
    PROFILESTART();
    int base, resbase;
    register T dot;
#pragma omp parallel for private(base, resbase, dot)
    for (int vv = 0; vv < n_vecs; vv++) {
        resbase = vv * stride;
        base = resbase * DIM;
        for (int s = 0; s < stride; s++) {
            dot = 0.0;
            for(int dd=0; dd<DIM;++dd)
                dot  += u_in[base + s + dd*stride] * v_in[base + s + dd*stride];
            
            x_out[resbase + s] = dot;
        }
    }

    PROFILEEND("CPU",0);
    return x_out;
}

template<>
template<typename T>  
T* Device<CPU>::CrossProduct(const T* u_in, const T* v_in, size_t stride, size_t num_surfs, T* w_out) const
{
    PROFILESTART();
    assert(DIM==3);
    
#pragma omp parallel 
    {
        T u[DIM], v[DIM], w[DIM];
        size_t base, resbase, surf, s;
        
#pragma omp for
        for (surf = 0; surf < num_surfs; surf++) {
            resbase = surf * stride;
            base = resbase * DIM;
            for(s = 0; s < stride; s++) {
                for(int dd=0;dd<DIM;++dd)
                {
                    u[dd] = u_in[base + s + dd * stride];
                    v[dd] = v_in[base + s + dd * stride];
                }

                w[0] = u[1] * v[2] - u[2] * v[1];
                w[1] = u[2] * v[0] - u[0] * v[2];
                w[2] = u[0] * v[1] - u[1] * v[0];
                
                for(int dd=0;dd<DIM;++dd)
                    w_out[base + s + dd * stride] = w[dd];
            }
        }
    }

    PROFILEEND("CPU",0);
    return w_out;
}

template<>
template<typename T>
T* Device<CPU>::Sqrt(const T* x_in, size_t length, T* sqrt_out) const
{
    PROFILESTART();
#pragma omp parallel for 
    for (int idx = 0; idx < length; idx++)
    {
        assert(x_in[idx] >= (T) 0.0);
        sqrt_out[idx] = ::sqrt(x_in[idx]);
    }
    PROFILEEND("CPU",0);
    return sqrt_out;
}

template<>
template<typename T>
T* Device<CPU>::ax(const T* a, const T* x, size_t stride, size_t n_vecs, T* ax_out) const
{
    PROFILESTART();
        
#pragma omp parallel for
    for(size_t n = 0;  n < n_vecs; ++n)
        for (size_t idx = 0; idx < stride; ++idx)
            ax_out[n*stride + idx]  = a[idx] * x[n * stride + idx];
    

    PROFILEEND("CPU",0);
    return ax_out;
}

template<>
template<typename T>
T* Device<CPU>::xy(const T* x_in, const T* y_in, size_t length, T* xy_out) const
{
    PROFILESTART();
    register T xTy;

#pragma omp parallel for private(xTy)
    for (size_t idx = 0; idx < length; idx++)
    {
        xTy  = x_in[idx];
        xTy *= y_in[idx];
        xy_out[idx] = xTy;
    }

    PROFILEEND("CPU",0);
    return xy_out;
}

template<>
template<typename T>
T* Device<CPU>::xyInv(const T* x_in, const T* y_in, size_t length, T* xyInv_out) const
{
    PROFILESTART();
    register T xDy;

    if(x_in == NULL)
    {
#pragma omp parallel for private(xDy)
        for (size_t idx = 0; idx < length; idx++)
        {
            assert( y_in[idx] != (T) 0.0);
            xDy  = 1.0;
            xDy /= y_in[idx];
            
            xyInv_out[idx] = xDy;
        }
    }
    else
    {
#pragma omp parallel for private(xDy)
        for (size_t idx = 0; idx < length; idx++)
        {
            assert( y_in[idx] != (T) 0.0);
            xDy  = x_in[idx];
            xDy /= y_in[idx];
            
            xyInv_out[idx] = xDy;
        }
    }

    PROFILEEND("CPU",0);
    return xyInv_out;
}

template<>
template<typename T>
T*  Device<CPU>::uyInv(const T* u_in, const T* y_in, size_t stride, size_t num_surfs, T* uyInv_out) const
{
    PROFILESTART();
    assert(u_in!=NULL);
    
    size_t y_base, base, y_idx;
    register T uy;
    
#pragma omp parallel for private(y_base, base, y_idx, uy)
    for (size_t vec = 0; vec < num_surfs; vec++)
    {
        y_base = vec*stride;
        base = DIM*y_base;
        for (size_t s = 0; s < stride; s++) 
        {
            y_idx = y_base + s;
            for(int dd=0;dd<DIM;++dd)
            {
                uy  = u_in[base + s + dd*stride];
                uy /= y_in[y_idx];
                uyInv_out[base + s + dd*stride] = uy;
            }
        }
    }
    
    PROFILEEND("CPU",0);
    return uyInv_out;
}

template<>
template<typename T>
T*  Device<CPU>::axpy(T a_in, const T*  x_in, const T*  y_in, size_t length, T*  axpy_out) const
{
    PROFILESTART();
    assert(x_in != NULL);
    
    register T val;
    
    if(y_in !=NULL)
    {
#pragma omp parallel for private(val)
        for (size_t idx = 0; idx < length; idx++)
        {
            val  = a_in;
            val *= x_in[idx];
            val += y_in[idx];
            axpy_out[idx] = val;
        }
    }
    else
    {
#pragma omp parallel for private(val)
        for (size_t idx = 0; idx < length; idx++)
        {
            val  = a_in;
            val *= x_in[idx];
            axpy_out[idx] = val;
        }
    }    

    PROFILEEND("CPU",0);
    return axpy_out;
}

template<>
template<typename T>
T*  Device<CPU>::apx(T* a_in, const T* x_in, size_t stride, 
    size_t n_subs, T* apx_out) const
{
    PROFILESTART();
    assert(a_in != NULL);
    assert(x_in != NULL);
    
#pragma omp parallel for
    for (size_t ii = 0; ii < n_subs; ++ii)
        for(size_t jj=0; jj < stride; ++jj)
            apx_out[ii * stride + jj] = a_in[ii] + x_in[ii * stride + jj];
           

    PROFILEEND("CPU",0);
    return apx_out;
}

template<>
template<typename T>
T*  Device<CPU>::avpw(const T* a_in, const T*  v_in, const T*  w_in, size_t stride, size_t num_surfs, T*  avpw_out) const
{
    PROFILESTART();
    register T val;
    size_t base, vec, s, length = DIM*stride, idx;

    if(w_in !=NULL)
    {
#pragma omp parallel for private(val, base, vec, s, idx)
        for (vec = 0; vec < num_surfs; vec++)
        {
            base = vec*length;        
            for (s = 0; s < stride; s++) {
            
                idx  = base+s;
                val  = a_in[vec];
                val *= v_in[idx];
                val += w_in[idx];
                avpw_out[idx] = val;

                idx +=stride;
                val  = a_in[vec];
                val *= v_in[idx];
                val += w_in[idx];
                avpw_out[idx] = val;
            
                idx +=stride;
                val  = a_in[vec];
                val *= v_in[idx];
                val += w_in[idx];
                avpw_out[idx] = val;
            }
        }
    }
    else
    {
#pragma omp parallel for private(val, base, vec, s, idx)
        for (vec = 0; vec < num_surfs; vec++)
        {
            base = vec*length;        
            for (s = 0; s < stride; s++) {
            
                idx  = base+s;
                val  = a_in[vec];
                val *= v_in[idx];
                avpw_out[idx] = val;

                idx +=stride;
                val  = a_in[vec];
                val *= v_in[idx];
                avpw_out[idx] = val;
            
                idx +=stride;
                val  = a_in[vec];
                val *= v_in[idx];
                avpw_out[idx] = val;
            }
        }
    }

    PROFILEEND("CPU",0);
    return avpw_out;
}

template<>
template<typename T>
T*  Device<CPU>::xvpw(const T* x_in, const T*  v_in, const T*  w_in, size_t stride, size_t num_surfs, T*  xvpw_out) const
{
    PROFILESTART();
    size_t base, x_base, vec, s, length = DIM*stride, idx, x_idx;
    register T val;

    if(w_in !=NULL)
    {
#pragma omp parallel for private(base, x_base, vec, s, idx, x_idx, val)
    for (vec = 0; vec < num_surfs; vec++)
    {
        base = vec*length;
        x_base = vec*stride;
        
        for (s = 0; s < stride; s++)
        {
            idx = base+s;
            x_idx = x_base+s;
            
            val  = x_in[x_idx];
            val *= v_in[idx];
            val += w_in[idx];
            xvpw_out[idx]  = val;

            idx +=stride;
            val  = x_in[x_idx];
            val *= v_in[idx];
            val += w_in[idx];
            xvpw_out[idx]  = val;

            idx +=stride;
            val  = x_in[x_idx];
            val *= v_in[idx];
            val += w_in[idx];
            xvpw_out[idx]  = val;
        }
    }

    }
    else
    {

#pragma omp parallel for private(base, x_base, vec, s, idx, x_idx, val)
    for (vec = 0; vec < num_surfs; vec++)
    {
        base = vec*length;
        x_base = vec*stride;
        
        for (s = 0; s < stride; s++)
        {
            idx = base+s;
            x_idx = x_base+s;
            
            val  = x_in[x_idx];
            val *= v_in[idx];
            xvpw_out[idx]  = val;

            idx +=stride;
            val  = x_in[x_idx];
            val *= v_in[idx];
            xvpw_out[idx]  = val;

            idx +=stride;
            val  = x_in[x_idx];
            val *= v_in[idx];
            xvpw_out[idx]  = val;
        }
    }
    }

    PROFILEEND("CPU",0);
    return xvpw_out;
}

template<>
template<typename T>
T*  Device<CPU>::Reduce(const T *x_in, const int x_dim, const T *w_in, const T *quad_w_in, 
    const size_t stride, const size_t ns, T *x_dw) const
{
    PROFILESTART();
    register T val;
    T sum;
    
    if(x_in != NULL)
    {
#pragma omp parallel for private(val,sum)
        for (size_t ii = 0; ii < ns; ++ii)
        {
            int wbase = ii * stride;
            for(int kk = 0; kk < x_dim; ++kk)
            {
                sum = 0;
                int xbase = ii * x_dim * stride + kk * stride;
                
                for (size_t jj = 0; jj < stride; ++jj) 
                {
                    val  = x_in[xbase + jj];
                    val *= w_in[wbase + jj];
                    val *= quad_w_in[jj];
                    
                    sum += val;
                }
                x_dw[ii*x_dim + kk] = sum;
            }
        }
    } else{
#pragma omp parallel for private(val,sum)
        for (size_t ii = 0; ii < ns; ++ii)
        {
            sum = 0;
            for (size_t jj = 0; jj < stride; ++jj) 
            {
                val = w_in[ii * stride + jj];
                val *= quad_w_in[jj];
                
                sum += val;
            }

           x_dw[ii] = sum;
        }
    }

    PROFILEEND("CPU",0);
    return x_dw;
}

template<>
float* Device<CPU>::gemm(const char *transA, const char *transB, 
    const int *m, const int *n, const int *k, const float *alpha, 
    const float *A, const int *lda, const float *B, const int *ldb, 
    const float *beta, float *C, const int *ldc) const
{
    PROFILESTART();
    sgemm(transA, transB, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
    PROFILEEND("CPUs", (double) 2* (*k) * (*n) * (*m) + *(beta) * (*n) * (*m));
    return C;
}

template<>
double* Device<CPU>::gemm(const char *transA, const char *transB, 
    const int *m, const int *n, const int *k, const double *alpha, 
    const double *A, const int *lda, const double *B, const int *ldb, 
    const double *beta, double *C, const int *ldc) const
{
    PROFILESTART();
    dgemm(transA, transB, m, n, k, alpha, A, lda, B, ldb, beta, C, ldc);
    PROFILEEND("CPUd",(double) 2* (*k) * (*n) * (*m) + *(beta) * (*n) * (*m));
    return C;
}

template<>
void Device<CPU>::DirectStokes(const double *src, const double *den, const double *qw, 
    size_t stride, size_t n_surfs, const double *trg, size_t trg_idx_head, 
    size_t trg_idx_tail, double *pot) const
{                  
    PROFILESTART();         
    if(qw != NULL)
        DirectStokesKernel(stride, n_surfs, trg_idx_head, 
            trg_idx_tail, qw, trg, src, den, pot);
    else
        DirectStokesKernel_Noqw(stride, n_surfs, trg_idx_head, 
            trg_idx_tail, trg, src, den, pot);
    
    PROFILEEND("CPUd",0);
    return;
} 

template<>
void Device<CPU>::DirectStokes(const float *src, const float *den, const float *qw, 
    size_t stride, size_t n_surfs, const float *trg, size_t trg_idx_head, 
    size_t trg_idx_tail, float *pot) const
{
    PROFILESTART();

#ifdef __SSE2__ 
    if(qw != NULL)
        DirectStokesSSE(stride, n_surfs, trg_idx_head, trg_idx_tail, 
            qw, trg, src, den, pot);
    else
        DirectStokesSSE_Noqw(stride, n_surfs, trg_idx_head, 
            trg_idx_tail, trg, src, den, pot);
    PROFILEEND("CPUfSSE",0);
#else
    CERR("The SSE instructions in the code are not compatible with this architecture.",endl);
    PROFILEEND("CPUf",0);
#endif   
}

template<>
template<typename T>
T Device<CPU>::MaxAbs(T *x_in, size_t length) const
{ 
    PROFILESTART();
    T *max_arr;
    int n_threads;
    
#pragma omp parallel
    {
        if(omp_get_thread_num() == 0)
            max_arr= (T*) this->Malloc(omp_get_num_threads() * sizeof(T));
        
        T max_loc = abs(*x_in);
#pragma omp for
        for(size_t idx = 0;idx<length;idx++)
            max_loc = (max_loc > abs(x_in[idx])) ? max_loc : abs(x_in[idx]);
        max_arr[omp_get_thread_num()] =  max_loc;
        
        if(omp_get_thread_num() == 0)
            n_threads = omp_get_num_threads();
    }
    
    T max=max_arr[0];
    for(size_t idx = 0;idx<n_threads;idx++)
        max = (max > max_arr[idx]) ? max : max_arr[idx];
    
    Free(max_arr);

    PROFILEEND("CPU",0);
    return(max);
}

template<>
template<typename T>
T* Device<CPU>::Transpose(const T *in, size_t height, size_t width, T *out) const
{ 
    assert(out != in);
    PROFILESTART();
    
#pragma omp parallel for
    for(size_t jj=0;jj<width;++jj)
        for(size_t ii=0;ii<height;++ii)
            out[jj*height + ii] = in[ii * width + jj];

    PROFILEEND("CPU",0);
    return(out);
}

template<>
template<typename T>
T Device<CPU>::AlgebraicDot(const T* x, const T* y, size_t length) const
{
    T dot(0.0);
    PROFILESTART();
    
#pragma omp parallel for reduction(+:dot)
    for(size_t idx=0;idx<length; ++idx)   
        dot = dot + (x[idx] * y[idx]);
    
    PROFILEEND("CPU",0);
    return(dot);
}

template<>
template<typename T>
bool Device<CPU>::isNan(const T* x, size_t length) const
{
    PROFILESTART();
    bool is_nan(false);

#pragma omp parallel for reduction(&&:is_nan)
    for(size_t idx=0;idx<length; ++idx)   
        is_nan = is_nan && ( x[idx] != x[idx] );
    
    PROFILEEND("CPU", 0);
    return is_nan;
}

template<>
template<typename T>
void Device<CPU>::fillRand(T* x, size_t length) const
{
    PROFILESTART();
    for(size_t idx=0;idx<length; ++idx)   
        x[idx] = static_cast<T>(drand48());
    PROFILEEND("CPU", 0);
}

template<enum DeviceType DTlhs, enum DeviceType DTrhs>                         
inline bool operator==(const Device<DTlhs> &lhs, const Device<DTrhs> &rhs)
{
    return((void*) &lhs == (void*) &rhs);
}
