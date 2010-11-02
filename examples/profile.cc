#include "EvolveSurface.h"

typedef float real;
#define DT GPU
extern const Device<DT> the_device(0);

template<const Device<DT> &DEVICE>
void EvolveSurfaceTest(Parameters<real> &sim_par)
{
    typedef EvolveSurface<real, DT, DEVICE> Evolve_t;
    typedef typename Evolve_t::Vec_t Vec_t;

    //Initial vesicle positions 
    Vec_t x0(sim_par.n_surfs, sim_par.sh_order);
    
    //reading the prototype form file
    PROFILESTART();
    DataIO myIO;
    char fname[300];
    sprintf(fname,"precomputed/biconcave_ra95_%u",sim_par.sh_order);
    myIO.ReadData(fname, x0, 0, x0.getSubLength());

    //Making Centers And Populating The Prototype
    int nVec = sim_par.n_surfs;
    real* cntrs_host =  new real[nVec * DIM];
    for(int ii=0; ii<nVec; ++ii)
    {
        cntrs_host[DIM*ii    ] = 0;
        cntrs_host[DIM*ii + 1] = 3*ii;
        cntrs_host[DIM*ii + 2] = 0;
    }
    Array<real, DT, DEVICE> cntrs(DIM * sim_par.n_surfs);
    cntrs.getDevice().Memcpy(cntrs.begin(), cntrs_host,
        cntrs.size() * sizeof(real), MemcpyHostToDevice);
    Populate(x0, cntrs);

    //Reading Operators From File
    bool readFromFile = true;
    typename Evolve_t::Mats_t Mats(readFromFile, sim_par);

    //Setting the background flow
    ShearFlow<Vec_t> vInf(sim_par.bg_flow_param);

    //Finally, Evolve surface
    Evolve_t Es(sim_par, Mats, x0, &vInf);
    PROFILEEND("Setup_",0);
    
    QC ( Es.Evolve() );
}

int main(int argc, char **argv)
{
    COUT("\n\n ========================\n  Profiling: "
        <<"\n ========================"<<endl);

    typedef Parameters<real> Par_t;
 
    // Setting the parameters
    Par_t sim_par;
    sim_par.n_surfs = 0;   
    sim_par.ts = .1;    
    sim_par.time_horizon = .3;
    sim_par.bg_flow_param = 0.01;
    sim_par.rep_maxit = 20;
    sim_par.save_data = false;    
    sim_par.scheme = Explicit;
    sim_par.singular_stokes = DirectEagerEval;
    COUT(sim_par);
    
    //Different p's
    int p[] = {6, 12, 16, 24};
    int plength = 4;
    int n = 512;
    
    sim_par.n_surfs = n; 
    for(int ii=0; ii<plength; ++ii)
    {
        sim_par.sh_order = p[ii];
        sim_par.filter_freq = 2*p[ii]/3;
        sim_par.rep_up_freq = 2*p[ii];
        sim_par.rep_filter_freq = p[ii]/3;
 
        COUT("\n --- n = " << n <<" p = "
            <<p[ii]<<"----------------------------------------"
            <<"-----------------------------------------------------"<<endl);
        COUT(sim_par);
        CLEARERRORHIST();
        PROFILESTART();
        
        EvolveSurfaceTest<the_device>(sim_par);
        PROFILEEND("",0);
        PRINTERRORLOG();
        PROFILEREPORT(SortFlopRate);
    }
}


