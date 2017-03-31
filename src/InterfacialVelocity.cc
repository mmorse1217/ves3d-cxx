template<typename SurfContainer, typename Interaction>
InterfacialVelocity<SurfContainer, Interaction>::
InterfacialVelocity(SurfContainer &S_in, const Interaction &Inter,
    const OperatorsMats<Arr_t> &mats,
    const Parameters<value_type> &params, const VProp_t &ves_props,
    const BgFlowBase<Vec_t> &bgFlow, PSolver_t *parallel_solver) :
    S_(S_in),
    interaction_(Inter),
    bg_flow_(bgFlow),
    params_(params),
    ves_props_(ves_props),
    Intfcl_force_(params,ves_props_,mats),
    //
    parallel_solver_(parallel_solver),
    psolver_configured_(false),
    precond_configured_(false),
    parallel_matvec_(NULL),
    parallel_rhs_(NULL),
    parallel_u_(NULL),
    //
    dt_(params_.ts),
    sht_(mats.p_, mats.mats_p_),
    sht_upsample_(mats.p_up_, mats.mats_p_up_),
    checked_out_work_sca_(0),
    checked_out_work_vec_(0),
    stokes_(params_.sh_order,params_.upsample_freq,params_.periodic_length,params_.repul_dist),
    S_up_(NULL)
{
    pos_vel_.replicate(S_.getPosition());
    tension_.replicate(S_.getPosition());

    pos_vel_.getDevice().Memset(pos_vel_.begin(), 0, sizeof(value_type)*pos_vel_.size());
    tension_.getDevice().Memset(tension_.begin(), 0, sizeof(value_type)*tension_.size());

    //Setting initial tension to zero
    tension_.getDevice().Memset(tension_.begin(), 0,
        tension_.size() * sizeof(value_type));

    int p = S_.getPosition().getShOrder();
    int np = S_.getPosition().getStride();

    //W_spherical
    w_sph_.resize(1, p);
    w_sph_inv_.resize(1, p);
    w_sph_.getDevice().Memcpy(w_sph_.begin(), mats.w_sph_,
        np * sizeof(value_type), device_type::MemcpyDeviceToDevice);
    xInv(w_sph_,w_sph_inv_);

    //Singular quadrature weights
    sing_quad_weights_.resize(1,p);
    sing_quad_weights_.getDevice().Memcpy(sing_quad_weights_.begin(),
        mats.sing_quad_weights_, sing_quad_weights_.size() *
        sizeof(value_type),
        device_type::MemcpyDeviceToDevice);

    //quadrature weights
    quad_weights_.resize(1,p);
    quad_weights_.getDevice().Memcpy(quad_weights_.begin(),
        mats.quad_weights_,
        quad_weights_.size() * sizeof(value_type),
        device_type::MemcpyDeviceToDevice);

    int p_up = sht_upsample_.getShOrder();
    quad_weights_up_.resize(1, p_up);
    quad_weights_up_.getDevice().Memcpy(quad_weights_up_.begin(),
        mats.quad_weights_p_up_,
        quad_weights_up_.size() * sizeof(value_type),
        device_type::MemcpyDeviceToDevice);

    //spectrum in harmonic space, diagonal
    if (params_.time_precond == DiagonalSpectral){
        position_precond.resize(1,p);
        tension_precond.resize(1,p);
    }
    
    // init Contact Interface
    std::vector<value_type> pos_init;
    pos_init.resize(S_.getPosition().size(),0.0);
    
    S_.getPosition().getDevice().Memcpy(&pos_init.front(), S_.getPosition().begin(),
        S_.getPosition().size() * sizeof(value_type),
        S_.getPosition().getDevice().MemcpyDeviceToDevice);
    
    CI_.generateMesh(pos_init,S_.getPosition().getShOrder(),S_.getPosition().getNumSubs());
    CI_.writeOFF();
    CI_.init(SURF_SUBDIVISION);
    vgrad_.replicate(S_.getPosition());
    vgrad_ind_.resize(S_.getPosition().size(),0);
    PA_.clear();
    num_cvs_ = 0;
    // end of init Contact Interface

    // init GMRES solver
    CHK(linear_solver_gmres_.SetContext(static_cast<const void*>(this)));
}

template<typename SurfContainer, typename Interaction>
InterfacialVelocity<SurfContainer, Interaction>::
~InterfacialVelocity()
{
    COUTDEBUG("Destroying an instance of interfacial velocity");
    assert(!checked_out_work_sca_);
    assert(!checked_out_work_vec_);

    purgeTheWorkSpace();

    COUTDEBUG("Deleting parallel matvec and containers");
    delete parallel_matvec_;
    delete parallel_rhs_;
    delete parallel_u_;

    if(S_up_) delete S_up_;
}

// Performs the following computation:
// velocity(n+1) = updateFarField( bending(n), tension(n) );    // Far-field
// velocity(n+1)+= stokes( bending(n) );                        // Add near-field due to bending
// tension(n+1)  = getTension( velocity(n+1) )                  // Linear solve to compute new tension
// velocity(n+1)+= stokes( tension(n+1) )                       // Add near-field due to tension
// position(n+1) = position(n) + dt*velocity(n+1)
//
// Notes: tension solve is block implicit.
template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
updateJacobiExplicit(const SurfContainer& S_, const value_type &dt, Vec_t& dx)
{
    this->dt_ = dt;
    SolverScheme scheme(JacobiBlockExplicit);
    INFO("Taking a time step using "<<scheme<<" scheme");
    CHK(Prepare(scheme));

    std::auto_ptr<Vec_t> u1 = checkoutVec();
    std::auto_ptr<Vec_t> u2 = checkoutVec();

    // puts u_inf and interaction in pos_vel_
    this->updateFarField();

    // add S[f_b]
    Intfcl_force_.bendingForce(S_, *u1);
    CHK(stokes(*u1, *u2));
    axpy(static_cast<value_type>(1.0), *u2, pos_vel_, pos_vel_);

    // compute tension
    CHK(getTension(pos_vel_, tension_));

    // add S[f_sigma]
    Intfcl_force_.tensileForce(S_, tension_, *u1);
    CHK(stokes(*u1, *u2));
    axpy(static_cast<value_type>(1.0), *u2, pos_vel_, pos_vel_);

    //axpy(dt_, pos_vel_, S_.getPosition(), S_.getPositionModifiable());
    dx.replicate(S_.getPosition());
    axpy(dt_, pos_vel_, dx);

    recycle(u1);
    recycle(u2);

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
updateJacobiGaussSeidel(const SurfContainer& S_, const value_type &dt, Vec_t& dx)
{
    this->dt_ = dt;
    SolverScheme scheme(JacobiBlockGaussSeidel);
    INFO("Taking a time step using "<<scheme<<" scheme");
    CHK(Prepare(scheme));

    std::auto_ptr<Vec_t> u1 = checkoutVec();
    std::auto_ptr<Vec_t> u2 = checkoutVec();
    std::auto_ptr<Vec_t> u3 = checkoutVec();

    // put far field in pos_vel_ and the sum with S[f_b] in u1
    this->updateFarField();
    Intfcl_force_.bendingForce(S_, *u2);
    CHK(stokes(*u2, *u1));
    axpy(static_cast<value_type>(1.0), pos_vel_, *u1, *u1);

    // tension
    CHK(getTension(*u1, tension_));
    Intfcl_force_.tensileForce(S_, tension_, *u1);
    CHK(stokes(*u1, *u2));

    // position rhs
    axpy(static_cast<value_type>(1.0), pos_vel_, *u2, *u1);
    axpy(dt_, *u1, S_.getPosition(), *u1);

    // initial guess
    u2->getDevice().Memcpy(u2->begin(), S_.getPosition().begin(),
        S_.getPosition().size() * sizeof(value_type),
        u2->getDevice().MemcpyDeviceToDevice);

    int iter(params_.time_iter_max);
    int rsrt(params_.time_iter_max);
    value_type tol(params_.time_tol),relres(params_.time_tol);

    enum BiCGSReturn solver_ret;
    Error_t ret_val(ErrorEvent::Success);

    COUTDEBUG("Solving for position");
    solver_ret = linear_solver_vec_(*this, *u2, *u1, rsrt, iter, relres);
    if ( solver_ret  != BiCGSSuccess )
        ret_val = ErrorEvent::DivergenceError;

    COUTDEBUG("Position solve: Total iter = "<<iter<<", relres = "<<tol);
    COUTDEBUG("Checking true relres");
    ASSERT(((*this)(*u2, *u3),
            axpy(static_cast<value_type>(-1), *u3, *u1, *u3),
            relres = sqrt(AlgebraicDot(*u3, *u3))/sqrt(AlgebraicDot(*u1,*u1)),
            relres<tol
           ),
           "relres ("<<relres<<")<tol("<<tol<<")"
           );

    //u2->getDevice().Memcpy(S_.getPositionModifiable().begin(), u2->begin(),
    //    S_.getPosition().size() * sizeof(value_type),
    //    u2->getDevice().MemcpyDeviceToDevice);
    dx.replicate(S_.getPosition());
    axpy(-1, S_.getPosition(), *u2, dx);

    recycle(u1);
    recycle(u2);
    recycle(u3);

    return ret_val;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
updateJacobiImplicit(const SurfContainer& S_, const value_type &dt, Vec_t& dx)
{
    this->dt_ = dt;
    SolverScheme scheme(JacobiBlockImplicit);
    INFO("Taking a time step using "<<scheme<<" scheme");
    
    // prepare scheme
    CHK(Prepare(scheme));
    
    // check out working vec, sca
    std::auto_ptr<Vec_t> x1 = checkoutVec();
    std::auto_ptr<Vec_t> f1 = checkoutVec();
    std::auto_ptr<Vec_t> b1 = checkoutVec();
    std::auto_ptr<Vec_t> xtmp = checkoutVec();

    std::auto_ptr<Sca_t> b2 = checkoutSca();
    
    // resize
    x1->replicate(S_.getPosition());
    f1->replicate(S_.getPosition());
    b1->replicate(S_.getPosition());
    xtmp->replicate(S_.getPosition());
    b2->replicate(tension_);
    
    // initial guess
    x1->getDevice().Memcpy(x1->begin(), pos_vel_.begin(),
        pos_vel_.size() * sizeof(value_type),
        x1->getDevice().MemcpyDeviceToDevice);

    // farfield velocity
    this->updateFarField();
    
    // position rhs
    Intfcl_force_.bendingForce(S_, *f1);
    stokes_.SetDensitySL(f1.get());
    stokes_.SetDensityDL(NULL);
    stokes_.SelfInteraction(*b1);
    axpy(static_cast<value_type>(1.0), pos_vel_, *b1, *b1);

    // tension rhs
    S_.div(*b1, *b2);
    axpy(static_cast<value_type>(-1.0),*b2,*b2);

    int iter(params_.time_iter_max);
    int rsrt(params_.time_iter_max);
    value_type tol(params_.time_tol),relres(params_.time_tol);

    Error_t ret_val(ErrorEvent::Success);
    COUTDEBUG("Solving for velocity");
        
    /* old code using BiCGStab Solver
    enum BiCGSReturnTMP solver_ret;
    solver_ret = linear_solver_vec_sca_(*this, *x1, tension_, *b1, *b2, rsrt, iter, relres);
    if ( solver_ret  != BiCGSSuccessTMP )
        ret_val = ErrorEvent::DivergenceError;
    INFO(solver_ret);
    */
    
    // new code using GMRES Solver
    size_t vsz(stokesBlockSize()), tsz(tensionBlockSize());
    ASSERT(S_.getPosition().size()==vsz,"Bad sizes");
    size_t N_size = vsz+tsz;
    // copy device type to value_type array to call GMRES
    value_type x_host[N_size], rhs_host[N_size];

    // copy to unknown solution
    x1->getDevice().Memcpy(x_host, x1->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    tension_.getDevice().Memcpy(x_host+vsz, tension_.begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    
    // copy to rhs
    b1->getDevice().Memcpy(rhs_host    , b1->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    b2->getDevice().Memcpy(rhs_host+vsz, b2->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

    // solve the linear system using gmres
    int solver_ret = linear_solver_gmres_(JacobiImplicitApply, JacobiImplicitPrecond, x_host, rhs_host, 
            relres, relres*0, N_size, iter, 300);

    // copy host to device
    x1->getDevice().Memcpy(x1->begin(), x_host    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    tension_.getDevice().Memcpy(tension_.begin(), x_host+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    // end of new code using GMRES Solver

    // developing code for col
    INFO("Begin of contact resolving steps.");
    // reset contact force
    axpy(static_cast<value_type>(-0), S_.fc_, S_.fc_);
    
    // pos_s stores the start configuration
    // pos_e stores the end configuration
    // vGrad stores the gradient of contact volumes
    std::vector<value_type> pos_s;
    std::vector<value_type> pos_e;
    std::vector<value_type> vGrad;
    pos_s.resize(S_.getPosition().size(),0.0);
    pos_e.resize(S_.getPosition().size(),0.0);
    vGrad.resize(S_.getPosition().size(),0.0);
    
    // copy current position to start configuration
    S_.getPosition().getDevice().Memcpy(&pos_s.front(), S_.getPosition().begin(),
        S_.getPosition().size() * sizeof(value_type),
        S_.getPosition().getDevice().MemcpyDeviceToHost);
 
    // the candidate position and copy to end configuration
    axpy(dt_, *x1, S_.getPosition(), *xtmp);
    xtmp->getDevice().Memcpy(&pos_e.front(), xtmp->begin(),
        xtmp->size() * sizeof(value_type),
        xtmp->getDevice().MemcpyDeviceToHost);
   
    int resolveCount = 0;
    std::vector<value_type> IV;

    if(params_.periodic_length > 0)
    {
        TransferVesicle(pos_s, pos_e);
    }
    CI_.getVolumeAndGradient(IV, num_cvs_, vGrad, vgrad_ind_, pos_s, pos_e, params_.min_sep_dist, params_.periodic_length);
    
    // checkout workers if num_cvs_ > 0
    //if(num_cvs_ > 0)
    //{
        std::auto_ptr<Vec_t> col_dx = checkoutVec();
        std::auto_ptr<Vec_t> col_f = checkoutVec();
        std::auto_ptr<Sca_t> col_tension = checkoutSca();
        
        col_dx->replicate(*x1);
        col_f->replicate(*x1);
        col_tension->replicate(tension_);
    //}

    while(num_cvs_ > 0)
    {
        // copy contact volume gradient to vgrad_
        vgrad_.getDevice().Memcpy(vgrad_.begin(), &vGrad.front(), 
                vgrad_.size() * sizeof(value_type),
                vgrad_.getDevice().MemcpyHostToDevice);

        // col_lambda stores the force magnitude
        Arr_t col_lambda(num_cvs_);
        // cvs stores the contact volumes
        Arr_t cvs(num_cvs_);
        // copy contact volumes to cvs
        cvs.getDevice().Memcpy(cvs.begin(), &IV.front(), 
          num_cvs_ * sizeof(value_type),
          cvs.getDevice().MemcpyHostToDevice);

        // solve for u, tension, lambda using minmap LCP solver
        INFO("Begin of SolveLCP.");
        SolveLCP(*col_dx, *col_tension, col_lambda, cvs);
        INFO("End of SolveLCP.");
        
        // update velocity
        axpy(static_cast<value_type>(1.0), *col_dx, *x1, *x1);
        // update tension
        axpy(static_cast<value_type>(1.0), *col_tension, tension_, tension_);
        // get contact force
        CVJacobianTrans(col_lambda, *col_f);
        // accumulate contact force to S_.fc_
        axpy(static_cast<value_type>(1.0), *col_f, S_.fc_, S_.fc_);
    
        // get new candidate position
        axpy(dt_, *x1, S_.getPosition(), *xtmp);

        // copy to pos_e as end configuration
        xtmp->getDevice().Memcpy(&pos_e.front(), xtmp->begin(),
                xtmp->size() * sizeof(value_type),
                xtmp->getDevice().MemcpyDeviceToHost);
      
        resolveCount++;
        INFO("Col iter#: "<<resolveCount);
        INFO("cvs: "<<cvs);
        INFO("lambda: "<<col_lambda);
      
        // test if still have contact
    
        if(params_.periodic_length > 0)
        {
            S_.getPosition().getDevice().Memcpy(&pos_s.front(), S_.getPosition().begin(),
                    S_.getPosition().size() * sizeof(value_type),
                    S_.getPosition().getDevice().MemcpyDeviceToHost);
            TransferVesicle(pos_s, pos_e);
        }
        CI_.getVolumeAndGradient(IV, num_cvs_, vGrad, vgrad_ind_, pos_s, pos_e, params_.min_sep_dist, params_.periodic_length);
    }

    // recycle if we had workers
    //if(num_cvs_ > 0)
    //{
        recycle(col_dx);
        recycle(col_tension);
        recycle(col_f);
    //}
    // end of developing code for col 
        
    COUTDEBUG("Position solve: Total iter = "<<iter<<", relres = "<<tol);
    COUTDEBUG("Checking true relres");
    /*
    ASSERT(((*this)(*u2, *u3),
            axpy(static_cast<value_type>(-1), *u3, *u1, *u3),
            relres = sqrt(AlgebraicDot(*u3, *u3))/sqrt(AlgebraicDot(*u1,*u1)),
            relres<tol
           ),
           "relres ("<<relres<<")<tol("<<tol<<")"
           );
    */

    dx.replicate(S_.getPosition());
    axpy(dt_, *x1, dx);
    axpy(static_cast<value_type>(1.0), *x1, pos_vel_);

    INFO("vel maxabs: "<<MaxAbs(pos_vel_));

    recycle(x1);
    recycle(f1);
    recycle(b1);
    recycle(b2);
    recycle(xtmp);

    return ret_val;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
updateImplicit(const SurfContainer& S_, const value_type &dt, Vec_t& dx)
{
    PROFILESTART();
    this->dt_ = dt;
    SolverScheme scheme(GloballyImplicit);
    INFO("Taking a time step using "<<scheme<<" scheme");
    CHK(Prepare(scheme));

    if (params_.solve_for_velocity) {
        CHK(AssembleRhsVel(parallel_rhs_, dt_, scheme));
    } else {
        CHK(AssembleRhsPos(parallel_rhs_, dt_, scheme));
    }

    Error_t err=ErrorEvent::Success;
    if(err==ErrorEvent::Success) err=AssembleInitial(parallel_u_, dt_, scheme);
    if(err==ErrorEvent::Success) err=Solve(parallel_rhs_, parallel_u_, dt_, scheme);
    if(err==ErrorEvent::Success) err=Update(parallel_u_);

    if(0)
    if (params_.solve_for_velocity && !params_.pseudospectral){ // Save velocity field to VTK
      std::auto_ptr<Vec_t> vel_ = checkoutVec();
      std::auto_ptr<Sca_t> ten_ = checkoutSca();
      { // Set vel_, ten_
          typename PVec_t::iterator i(NULL);
          typename PVec_t::size_type rsz;
          CHK(parallel_u_->GetArray(i, rsz));
          size_t vsz(stokesBlockSize()), tsz(tensionBlockSize());
          ASSERT(rsz==vsz+tsz,"Bad sizes");

          vel_->replicate(pos_vel_);
          ten_->replicate(tension_);
          {
              std::auto_ptr<Vec_t> voxSh = checkoutVec();
              std::auto_ptr<Sca_t> tSh   = checkoutSca();
              std::auto_ptr<Vec_t> wrk   = checkoutVec();

              voxSh->replicate(*vel_);
              tSh->replicate(*ten_);
              wrk->replicate(*vel_);

              voxSh->getDevice().Memcpy(voxSh->begin(), i    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
              tSh  ->getDevice().Memcpy(tSh  ->begin(), i+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);

              sht_.backward(*voxSh, *wrk, *vel_);
              sht_.backward(*tSh  , *wrk, *ten_);

              recycle(voxSh);
              recycle(tSh);
              recycle(wrk);
          }
          CHK(parallel_u_->RestoreArray(i));
      }

      { // Set DensitySL
          std::auto_ptr<Vec_t> f   = checkoutVec();
          Intfcl_force_.explicitTractionJump(S_, *f);
          { // Add implicit traction jump
              std::auto_ptr<Vec_t> Du  = checkoutVec();
              std::auto_ptr<Vec_t> fi  = checkoutVec();
              axpy(dt_, *vel_, *Du);
              Intfcl_force_.implicitTractionJump(S_, *Du, *ten_, *fi);
              axpy(static_cast<value_type>(1.0), *fi, *f, *f);
              recycle(Du);
              recycle(fi);
          }
          stokes_.SetDensitySL(f.get());
          recycle(f);
      }

      if( ves_props_.has_contrast ){ // Set DensityDL
          std::auto_ptr<Vec_t> lcoeff_vel  = checkoutVec();
          av(ves_props_.dl_coeff, *vel_, *lcoeff_vel);
          stokes_.SetDensityDL(lcoeff_vel.get());
          recycle(lcoeff_vel);
      } else {
          stokes_.SetDensityDL(NULL);
      }

      if(0){ // Print error
        std::auto_ptr<Vec_t> Sf = checkoutVec();
        stokes_(*Sf);

        { // Add bg_vel
          std::auto_ptr<Vec_t> bg_vel = checkoutVec();
          bg_vel->replicate(S_.getPosition());
          CHK(BgFlow(*bg_vel, dt));
          axpy(static_cast<value_type>(1.0), *bg_vel, *Sf, *Sf);
          recycle(bg_vel);
        }
        if( ves_props_.has_contrast ){
          Arr_t* inv_vel_coeff = new Arr_t;
          inv_vel_coeff->resize(ves_props_.vel_coeff.size());
          //xInv(ves_props_.vel_coeff,*inv_vel_coeff);
          { // inv_vel_coeff = 1.0/ves_props_.vel_coeff
            const value_type* in=ves_props_.vel_coeff .begin();
            value_type*      out=       inv_vel_coeff->begin();
            for(long i=0;i<inv_vel_coeff->size();i++) out[i]=1.0/in[i];
          }
          av(*inv_vel_coeff, *Sf, *Sf);
          delete inv_vel_coeff;
        }

        axpy(static_cast<value_type>(-1.0), *vel_, *Sf, *Sf);
        pvfmm::Matrix<value_type> dv(1,Sf->size(),Sf->begin());
        pvfmm::Matrix<value_type> dv2=dv*dv.Transpose();

        pvfmm::Matrix<value_type> v(1, vel_->size(),vel_->begin());
        pvfmm::Matrix<value_type> v2=v*v.Transpose();

        std::cout<<"GMRES Error = "<<sqrt(dv2[0][0])<<"/"<<sqrt(v2[0][0])<<'\n';
        recycle(Sf);
      }else{ // Write VTK
        int myrank, np;
        MPI_Comm comm=MPI_COMM_WORLD;
        MPI_Comm_rank(comm,&myrank);
        MPI_Comm_size(comm,&np);

        typedef float VTKReal;
        struct VTKData{
          std::vector<VTKReal> coord;
          std::vector<VTKReal> value;

          std::vector<int32_t> connect;
          std::vector<int32_t> offset ;
          std::vector<uint8_t> types  ;
        };
        VTKData vtk_data;

        { // Set vtk_data
          value_type range[6];
          range[0]=-4; range[1]=-4; range[2]=-4;
          range[3]= 4; range[4]= 4; range[5]= 4;
          long gridpt_cnt=40;
          long data_dof=3;

          std::vector<VTKReal>& coord=vtk_data.coord;
          std::vector<VTKReal>& value=vtk_data.value;

          std::vector<int32_t>& connect=vtk_data.connect;
          std::vector<int32_t>& offset =vtk_data.offset ;
          std::vector<uint8_t>& types  =vtk_data.types  ;

          { // Set coord, connect, offset, types
            long Ngrid=(gridpt_cnt-1)*(gridpt_cnt-1)*(gridpt_cnt-1);
            long idx_start=(Ngrid*(myrank+0))/np;
            long idx_end  =(Ngrid*(myrank+1))/np;

            long grid_idx=0;
            for(int i0=0;i0<(gridpt_cnt-1);i0++)
            for(int i1=0;i1<(gridpt_cnt-1);i1++)
            for(int i2=0;i2<(gridpt_cnt-1);i2++){
              if(idx_start<=grid_idx && grid_idx<idx_end){
                for(int j0=0;j0<2;j0++)
                for(int j1=0;j1<2;j1++)
                for(int j2=0;j2<2;j2++){
                  connect.push_back(coord.size()/3);
                  coord.push_back((i0+j0)/(gridpt_cnt-1.0)*(range[3]-range[0])+range[0]);
                  coord.push_back((i1+j1)/(gridpt_cnt-1.0)*(range[4]-range[1])+range[1]);
                  coord.push_back((i2+j2)/(gridpt_cnt-1.0)*(range[5]-range[2])+range[2]);
                  for(int j=0;j<data_dof;j++) value.push_back(0.0);
                }
                offset.push_back(connect.size());
                types.push_back(11);
              }
              grid_idx++;
            }
          }

          { // Set vtk_data.value
            pvfmm::Vector<value_type> coord(vtk_data.coord.size()), value(vtk_data.value.size());
            for(long i=0;i<coord.Dim();i++) coord[i]=vtk_data.coord[i];
            stokes_.SetSrcCoord(S_.getPosition(),100,100);
            stokes_.SetTrgCoord(&coord);
            value=stokes_();
            { // Add BgVel
              long ntrg=coord.Dim()/COORD_DIM;
              long nv=(ntrg+4-1)/4;
              long mv=4;

              Vec_t c(nv,1), bgvel(nv,1);
              value_type* c_=c.begin();
              for(long i0=0;i0<nv;i0++){
                for(long i1=0;i1<mv;i1++){
                  long i=i0*mv+i1;
                  if(i<ntrg){
                    c_[(i0*COORD_DIM+0)*mv+i1]=coord[i*COORD_DIM+0];
                    c_[(i0*COORD_DIM+1)*mv+i1]=coord[i*COORD_DIM+1];
                    c_[(i0*COORD_DIM+2)*mv+i1]=coord[i*COORD_DIM+2];
                  }
                }
              }

              bg_flow_(c, 0, bgvel);
              value_type* bgvel_=bgvel.begin();
              for(long i0=0;i0<nv;i0++){
                for(long i1=0;i1<mv;i1++){
                  long i=i0*mv+i1;
                  if(i<ntrg){
                    value[i*COORD_DIM+0]+=bgvel_[(i0*COORD_DIM+0)*mv+i1];
                    value[i*COORD_DIM+1]+=bgvel_[(i0*COORD_DIM+1)*mv+i1];
                    value[i*COORD_DIM+2]+=bgvel_[(i0*COORD_DIM+2)*mv+i1];
                  }
                }
              }
            }
            stokes_.SetTrgCoord(NULL);
            for(long i=0;i<value.Dim();i++) vtk_data.value[i]=value[i];
          }
        }

        const char* fname="vis/vel";
        { // WriteVTK
          std::vector<VTKReal>& coord=vtk_data.coord;
          std::vector<VTKReal>& value=vtk_data.value;

          std::vector<int32_t>& connect=vtk_data.connect;
          std::vector<int32_t>& offset =vtk_data.offset;
          std::vector<uint8_t>& types  =vtk_data.types;

          int pt_cnt=coord.size()/3;
          int cell_cnt=types.size();

          std::vector<int32_t> mpi_rank;  //MPI_Rank at points.
          int new_myrank=myrank;//rand();
          mpi_rank.resize(pt_cnt,new_myrank);

          bool isLittleEndian;
          { // Set isLittleEndian
            uint16_t number = 0x1;
            uint8_t *numPtr = (uint8_t*)&number;
            isLittleEndian=(numPtr[0] == 1);
          }

          //Open file for writing.
          std::stringstream vtufname;
          vtufname<<fname<<std::setfill('0')<<std::setw(6)<<myrank<<".vtu";
          std::ofstream vtufile;
          vtufile.open(vtufname.str().c_str());
          if(!vtufile.fail()){ // write .vtu
            size_t data_size=0;
            vtufile<<"<?xml version=\"1.0\"?>\n";
            if(isLittleEndian) vtufile<<"<VTKFile type=\"UnstructuredGrid\" version=\"0.1\" byte_order=\"LittleEndian\">\n";
            else               vtufile<<"<VTKFile type=\"UnstructuredGrid\" version=\"0.1\" byte_order=\"BigEndian\">\n";
            //===========================================================================
            vtufile<<"  <UnstructuredGrid>\n";
            vtufile<<"    <Piece NumberOfPoints=\""<<pt_cnt<<"\" NumberOfCells=\""<<cell_cnt<<"\">\n";

            //---------------------------------------------------------------------------
            vtufile<<"      <Points>\n";
            vtufile<<"        <DataArray type=\"Float"<<sizeof(VTKReal)*8<<"\" NumberOfComponents=\""<<3<<"\" Name=\"Position\" format=\"appended\" offset=\""<<data_size<<"\" />\n";
            data_size+=sizeof(uint32_t)+coord.size()*sizeof(VTKReal);
            vtufile<<"      </Points>\n";
            //---------------------------------------------------------------------------
            vtufile<<"      <PointData>\n";
            if(value.size()){ // value
              vtufile<<"        <DataArray type=\"Float"<<sizeof(VTKReal)*8<<"\" NumberOfComponents=\""<<value.size()/pt_cnt<<"\" Name=\""<<"value"<<"\" format=\"appended\" offset=\""<<data_size<<"\" />\n";
              data_size+=sizeof(uint32_t)+value.size()*sizeof(VTKReal);
            }
            { // mpi_rank
              vtufile<<"        <DataArray type=\"Int32\" NumberOfComponents=\"1\" Name=\"mpi_rank\" format=\"appended\" offset=\""<<data_size<<"\" />\n";
              data_size+=sizeof(uint32_t)+mpi_rank.size()*sizeof(int32_t);
            }
            vtufile<<"      </PointData>\n";
            //---------------------------------------------------------------------------
            //---------------------------------------------------------------------------
            vtufile<<"      <Cells>\n";
            vtufile<<"        <DataArray type=\"Int32\" Name=\"connectivity\" format=\"appended\" offset=\""<<data_size<<"\" />\n";
            data_size+=sizeof(uint32_t)+connect.size()*sizeof(int32_t);
            vtufile<<"        <DataArray type=\"Int32\" Name=\"offsets\" format=\"appended\" offset=\""<<data_size<<"\" />\n";
            data_size+=sizeof(uint32_t)+offset.size() *sizeof(int32_t);
            vtufile<<"        <DataArray type=\"UInt8\" Name=\"types\" format=\"appended\" offset=\""<<data_size<<"\" />\n";
            data_size+=sizeof(uint32_t)+types.size()  *sizeof(uint8_t);
            vtufile<<"      </Cells>\n";
            //---------------------------------------------------------------------------
            //vtufile<<"      <CellData>\n";
            //vtufile<<"        <DataArray type=\"Float"<<sizeof(VTKReal)*8<<"\" Name=\"Velocity\" format=\"appended\" offset=\""<<data_size<<"\" />\n";
            //vtufile<<"      </CellData>\n";
            //---------------------------------------------------------------------------

            vtufile<<"    </Piece>\n";
            vtufile<<"  </UnstructuredGrid>\n";
            //===========================================================================
            vtufile<<"  <AppendedData encoding=\"raw\">\n";
            vtufile<<"    _";

            int32_t block_size;
            block_size=coord   .size()*sizeof(VTKReal); vtufile.write((char*)&block_size, sizeof(int32_t)); vtufile.write((char*)&coord   [0], coord   .size()*sizeof(VTKReal));
            if(value.size()){ // value
              block_size=value .size()*sizeof(VTKReal); vtufile.write((char*)&block_size, sizeof(int32_t)); vtufile.write((char*)&value   [0], value   .size()*sizeof(VTKReal));
            }
            block_size=mpi_rank.size()*sizeof(int32_t); vtufile.write((char*)&block_size, sizeof(int32_t)); vtufile.write((char*)&mpi_rank[0], mpi_rank.size()*sizeof(int32_t));

            block_size=connect .size()*sizeof(int32_t); vtufile.write((char*)&block_size, sizeof(int32_t)); vtufile.write((char*)&connect [0], connect .size()*sizeof(int32_t));
            block_size=offset  .size()*sizeof(int32_t); vtufile.write((char*)&block_size, sizeof(int32_t)); vtufile.write((char*)&offset  [0], offset  .size()*sizeof(int32_t));
            block_size=types   .size()*sizeof(uint8_t); vtufile.write((char*)&block_size, sizeof(int32_t)); vtufile.write((char*)&types   [0], types   .size()*sizeof(uint8_t));

            vtufile<<"\n";
            vtufile<<"  </AppendedData>\n";
            //===========================================================================
            vtufile<<"</VTKFile>\n";
            vtufile.close();
          }
          if(!myrank){ // write .pvtu
            std::stringstream pvtufname;
            pvtufname<<fname<<".pvtu";
            std::ofstream pvtufile;
            pvtufile.open(pvtufname.str().c_str());
            if(!pvtufile.fail()){
              pvtufile<<"<?xml version=\"1.0\"?>\n";
              pvtufile<<"<VTKFile type=\"PUnstructuredGrid\">\n";
              pvtufile<<"  <PUnstructuredGrid GhostLevel=\"0\">\n";
              pvtufile<<"      <PPoints>\n";
              pvtufile<<"        <PDataArray type=\"Float"<<sizeof(VTKReal)*8<<"\" NumberOfComponents=\""<<3<<"\" Name=\"Position\"/>\n";
              pvtufile<<"      </PPoints>\n";
              pvtufile<<"      <PPointData>\n";
              if(value.size()){ // value
                pvtufile<<"        <PDataArray type=\"Float"<<sizeof(VTKReal)*8<<"\" NumberOfComponents=\""<<value.size()/pt_cnt<<"\" Name=\""<<"value"<<"\"/>\n";
              }
              { // mpi_rank
                pvtufile<<"        <PDataArray type=\"Int32\" NumberOfComponents=\"1\" Name=\"mpi_rank\"/>\n";
              }
              pvtufile<<"      </PPointData>\n";
              {
                // Extract filename from path.
                std::stringstream vtupath;
                vtupath<<'/'<<fname;
                std::string pathname = vtupath.str();
                unsigned found = pathname.find_last_of("/\\");
                std::string fname_ = pathname.substr(found+1);
                //char *fname_ = (char*)strrchr(vtupath.str().c_str(), '/') + 1;
                //std::string fname_ = boost::filesystem::path(fname).filename().string().
                for(int i=0;i<np;i++) pvtufile<<"      <Piece Source=\""<<fname_<<std::setfill('0')<<std::setw(6)<<i<<".vtu\"/>\n";
              }
              pvtufile<<"  </PUnstructuredGrid>\n";
              pvtufile<<"</VTKFile>\n";
              pvtufile.close();
            }
          }
        }
      }

      recycle(vel_);
      recycle(ten_);
    }

    dx.replicate(S_.getPosition());
    if (params_.solve_for_velocity){
        axpy(dt, pos_vel_, dx);
    } else {
        axpy(-1.0, S_.getPosition(), pos_vel_, dx);
    }

    PROFILEEND("",0);
    return err;
}

template<typename SurfContainer, typename Interaction>
size_t InterfacialVelocity<SurfContainer, Interaction>::stokesBlockSize() const{

    return (params_.pseudospectral ?
        S_.getPosition().size() :
        S_.getPosition().getShOrder()*(S_.getPosition().getShOrder()+2)*S_.getPosition().getNumSubFuncs() ); /* (p+1)^2-1 (last freq doesn't have a cosine */
}

template<typename SurfContainer, typename Interaction>
size_t InterfacialVelocity<SurfContainer, Interaction>::tensionBlockSize() const{
    return stokesBlockSize()/3;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::Prepare(const SolverScheme &scheme) const
{
    PROFILESTART();

    if (pos_vel_.size() != S_.getPosition().size()){
    
      INFO("Zeroing pos_vel_ and tension_!!!!");
      COUTDEBUG("Resizing the containers");
      pos_vel_.replicate(S_.getPosition());
      tension_.replicate(S_.getPosition());

      COUTDEBUG("zeroing content of velocity and tension arrays");
      pos_vel_.getDevice().Memset(pos_vel_.begin(), 0, sizeof(value_type)*pos_vel_.size());
      tension_.getDevice().Memset(tension_.begin(), 0, sizeof(value_type)*tension_.size());
    }

    ASSERT(pos_vel_.size() == S_.getPosition().size(), "inccorrect size");
    ASSERT(3*tension_.size() == S_.getPosition().size(), "inccorrect size");
    ASSERT(ves_props_.dl_coeff.size() == S_.getPosition().getNumSubs(), "inccorrect size");
    ASSERT(ves_props_.vel_coeff.size() == S_.getPosition().getNumSubs(), "inccorrect size");
    ASSERT(ves_props_.bending_modulus.size() == S_.getPosition().getNumSubs(), "inccorrect size");

    INFO("Setting interaction source and target");
    stokes_.SetSrcCoord(S_.getPosition());

    if (!precond_configured_ && params_.time_precond!=NoPrecond)
        ConfigurePrecond(params_.time_precond);

    //!@bug doesn't support repartitioning
    if (!psolver_configured_ && scheme==GloballyImplicit){
        ASSERT(parallel_solver_ != NULL, "need a working parallel solver");
        CHK(ConfigureSolver(scheme));
    }

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
ConfigureSolver(const SolverScheme &scheme) const
{
    PROFILESTART();
    ASSERT(scheme==GloballyImplicit, "Unsupported scheme");
    COUTDEBUG("Configuring the parallel solver");

    typedef typename PSolver_t::matvec_type POp;
    typedef typename PSolver_t::vec_type PVec;
    typedef typename PVec::size_type size_type;

    // Setting up the operator
    size_type sz(stokesBlockSize() + tensionBlockSize());
    CHK(parallel_solver_->LinOpFactory(&parallel_matvec_));
    CHK(parallel_matvec_->SetSizes(sz,sz));
    CHK(parallel_matvec_->SetName("Vesicle interaction"));
    CHK(parallel_matvec_->SetContext(static_cast<const void*>(this)));
    CHK(parallel_matvec_->SetApply(ImplicitApply));
    CHK(parallel_matvec_->Configure());

    // setting up the rhs
    CHK(parallel_solver_->VecFactory(&parallel_rhs_));
    CHK(parallel_rhs_->SetSizes(sz));
    CHK(parallel_rhs_->SetName("rhs"));
    CHK(parallel_rhs_->Configure());

    CHK(parallel_rhs_->ReplicateTo(&parallel_u_));
    CHK(parallel_u_->SetName("solution"));

    // setting up the solver
    CHK(parallel_solver_->SetOperator(parallel_matvec_));
    CHK(parallel_solver_->SetTolerances(params_.time_tol,
            PSolver_t::PLS_DEFAULT,
            PSolver_t::PLS_DEFAULT,
            params_.time_iter_max));

    CHK(parallel_solver_->Configure());

    // setting up the preconditioner
    if (params_.time_precond != NoPrecond){
        ASSERT(precond_configured_, "The preconditioner isn't configured yet");
        CHK(parallel_solver_->SetPrecondContext(static_cast<const void*>(this)));
        CHK(parallel_solver_->UpdatePrecond(ImplicitPrecond));
    }
    psolver_configured_ = true;

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
ConfigurePrecond(const PrecondScheme &precond) const{

    PROFILESTART();
    if (precond!=DiagonalSpectral)
        return ErrorEvent::NotImplementedError; /* Unsupported preconditioner scheme */

    INFO("Setting up the diagonal preceonditioner");
    value_type *buffer = new value_type[position_precond.size() * sizeof(value_type)];

    { //bending precond
        int idx(0), N(0);
        // The sh coefficients are ordered by m and then n
        for(int iM=0; iM<position_precond.getGridDim().second; ++iM){
            for(int iN=++N/2; iN<position_precond.getGridDim().first; ++iN){
                value_type bending_precond(1.0/fabs(1.0-dt_*iN*iN*iN));
                bending_precond = fabs(bending_precond) < 1e3   ? bending_precond : 1.0;
                buffer[idx]     = fabs(bending_precond) > 1e-10 ? bending_precond : 1.0;
                ++idx;
            }
        }
        position_precond.getDevice().Memcpy(position_precond.begin(), buffer,
            position_precond.size() * sizeof(value_type),
            device_type::MemcpyHostToDevice);
    }
    { // tension precond
        int idx(0), N(0);
        for(int iM=0; iM<tension_precond.getGridDim().second; ++iM){
            for(int iN=++N/2; iN<tension_precond.getGridDim().first; ++iN){
                value_type eig(4*iN*iN-1);
                eig *= 2*iN+3;
                eig /= iN+1;
                eig /= 2*iN*iN+2*iN-1;
                eig  = iN==0 ? 1.0 : eig/iN;
                buffer[idx] = fabs(eig) > 1e-10 ? eig : 1.0;
                ++idx;
            }
        }
        tension_precond.getDevice().Memcpy(tension_precond.begin(), buffer,
            tension_precond.size() * sizeof(value_type),
            device_type::MemcpyHostToDevice);
    }

    delete[] buffer;
    precond_configured_=true;
    PROFILEEND("",0);

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
AssembleRhsVel(PVec_t *rhs, const value_type &dt, const SolverScheme &scheme) const
{
    PROFILESTART();
    ASSERT(scheme==GloballyImplicit, "Unsupported scheme");
    INFO("Assembling RHS to solve for velocity");

    // rhs=[u_inf+Bx;div(u_inf+Bx)]
    COUTDEBUG("Evaluate background flow");
    std::auto_ptr<Vec_t> vRhs = checkoutVec();
    vRhs->replicate(S_.getPosition());
    CHK(BgFlow(*vRhs, dt));

    COUTDEBUG("Computing the far-field interaction due to explicit traction jump");
    std::auto_ptr<Vec_t> f  = checkoutVec();
    std::auto_ptr<Vec_t> Sf = checkoutVec();
    Intfcl_force_.explicitTractionJump(S_, *f);
    stokes_.SetDensitySL(f.get(),true);
    stokes_.SetDensityDL(NULL);
    stokes_(*Sf);
    axpy(static_cast<value_type>(1.0), *Sf, *vRhs, *vRhs);

    COUTDEBUG("Computing rhs for div(u)");
    std::auto_ptr<Sca_t> tRhs = checkoutSca();
    S_.div(*vRhs, *tRhs);

    ASSERT( vRhs->getDevice().isNumeric(vRhs->begin(), vRhs->size()), "Non-numeric rhs");
    ASSERT( tRhs->getDevice().isNumeric(tRhs->begin(), tRhs->size()), "Non-numeric rhs");

    // copy data
    size_t xsz(stokesBlockSize()), tsz(tensionBlockSize());
    typename PVec_t::iterator i(NULL);
    typename PVec_t::size_type rsz;
    CHK(parallel_rhs_->GetArray(i, rsz));
    ASSERT(rsz==xsz+tsz,"Bad sizes");

    if (params_.pseudospectral){
        COUTDEBUG("Copy data to parallel rhs array");
        vRhs->getDevice().Memcpy(i    , vRhs->begin(), xsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        tRhs->getDevice().Memcpy(i+xsz, tRhs->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    } else {  /* Galerkin */
        COUTDEBUG("Project RHS to spectral coefficient");
        std::auto_ptr<Vec_t> vRhsSh  = checkoutVec();
        std::auto_ptr<Sca_t> tRhsSh  = checkoutSca();
        std::auto_ptr<Vec_t> wrk     = checkoutVec();

        vRhsSh->replicate(*vRhs);
        tRhsSh->replicate(*tRhs);
        wrk->replicate(*vRhs);

        sht_.forward(*vRhs, *wrk, *vRhsSh);
        sht_.forward(*tRhs, *wrk, *tRhsSh);

        COUTDEBUG("Copy data to parallel RHS array (size="<<xsz<<"+"<<tsz<<")");
        vRhs->getDevice().Memcpy(i    , vRhsSh->begin(), xsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        tRhs->getDevice().Memcpy(i+xsz, tRhsSh->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

        recycle(vRhsSh);
        recycle(tRhsSh);
        recycle(wrk);
    }

    CHK(parallel_rhs_->RestoreArray(i));

    recycle(vRhs);
    recycle(f);
    recycle(Sf);
    recycle(tRhs);

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
AssembleRhsPos(PVec_t *rhs, const value_type &dt, const SolverScheme &scheme) const
{
    PROFILESTART();
    ASSERT(scheme==GloballyImplicit, "Unsupported scheme");
    INFO("Assembling RHS to solve for position");

    COUTDEBUG("Evaluate background flow");
    std::auto_ptr<Vec_t> pRhs = checkoutVec();
    std::auto_ptr<Vec_t> pRhs2 = checkoutVec();
    pRhs->replicate(S_.getPosition());
    pRhs2->replicate(S_.getPosition());
    CHK(BgFlow(*pRhs, dt));

    if( ves_props_.has_contrast ){
        COUTDEBUG("Computing the rhs due to viscosity contrast");
        std::auto_ptr<Vec_t> x  = checkoutVec();
        std::auto_ptr<Vec_t> Dx = checkoutVec();
        av(ves_props_.dl_coeff, S_.getPosition(), *x);
        stokes_.SetDensitySL(NULL, true);
        stokes_.SetDensityDL(x.get());
        stokes_(*Dx);
        axpy(-dt, *pRhs, *Dx, *pRhs);
        axpy(static_cast<value_type>(-1.0), *pRhs, *pRhs);

        recycle(x);
        recycle(Dx);
    } else
        axpy(dt, *pRhs, *pRhs);

    COUTDEBUG("Computing rhs for div(u)");
    std::auto_ptr<Sca_t> tRhs = checkoutSca();
    S_.div(*pRhs, *tRhs);

    av(ves_props_.vel_coeff, S_.getPosition(), *pRhs2);
    axpy(static_cast<value_type>(1.0), *pRhs, *pRhs2, *pRhs);

    ASSERT( pRhs->getDevice().isNumeric(pRhs->begin(), pRhs->size()), "Non-numeric rhs");
    ASSERT( tRhs->getDevice().isNumeric(tRhs->begin(), tRhs->size()), "Non-numeric rhs");

    // copy data
    size_t xsz(stokesBlockSize()), tsz(tensionBlockSize());
    typename PVec_t::iterator i(NULL);
    typename PVec_t::size_type rsz;
    CHK(parallel_rhs_->GetArray(i, rsz));
    ASSERT(rsz==xsz+tsz,"Bad sizes");

    if (params_.pseudospectral){
        COUTDEBUG("Copy data to parallel rhs array");
        pRhs->getDevice().Memcpy(i    , pRhs->begin(), xsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        tRhs->getDevice().Memcpy(i+xsz, tRhs->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    } else {  /* Galerkin */
        COUTDEBUG("Project RHS to spectral coefficient");
        std::auto_ptr<Vec_t> pRhsSh  = checkoutVec();
        std::auto_ptr<Sca_t> tRhsSh  = checkoutSca();
        std::auto_ptr<Vec_t> wrk     = checkoutVec();

        pRhsSh->replicate(*pRhs);
        tRhsSh->replicate(*tRhs);
        wrk->replicate(*pRhs);

        sht_.forward(*pRhs, *wrk, *pRhsSh);
        sht_.forward(*tRhs, *wrk, *tRhsSh);

        COUTDEBUG("Copy data to parallel rhs array");
        pRhs->getDevice().Memcpy(i    , pRhsSh->begin(), xsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        tRhs->getDevice().Memcpy(i+xsz, tRhsSh->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

        recycle(pRhsSh);
        recycle(tRhsSh);
        recycle(wrk);
    }

    CHK(parallel_rhs_->RestoreArray(i));

    recycle(pRhs);
    recycle(pRhs2);
    recycle(tRhs);

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
AssembleInitial(PVec_t *u0, const value_type &dt, const SolverScheme &scheme) const
{
    PROFILESTART();
    COUTDEBUG("Using current position/tension as initial guess");
    size_t vsz(stokesBlockSize()), tsz(tensionBlockSize());
    typename PVec_t::iterator i(NULL);
    typename PVec_t::size_type rsz;

    ASSERT( pos_vel_.getDevice().isNumeric(pos_vel_.begin(), pos_vel_.size()), "Non-numeric velocity");
    ASSERT( tension_.getDevice().isNumeric(tension_.begin(), tension_.size()), "Non-numeric tension");

    CHK(parallel_u_->GetArray(i, rsz));
    ASSERT(rsz==vsz+tsz,"Bad sizes");

    if (params_.pseudospectral){
        COUTDEBUG("Copy initial guess to parallel solution array");
        pos_vel_.getDevice().Memcpy(i    , pos_vel_.begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        tension_.getDevice().Memcpy(i+vsz, tension_.begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    } else {  /* Galerkin */
            COUTDEBUG("Project initial guess to spectral coefficient");
        std::auto_ptr<Vec_t> voxSh  = checkoutVec();
        std::auto_ptr<Sca_t> tSh    = checkoutSca();
        std::auto_ptr<Vec_t> wrk    = checkoutVec();

        voxSh->replicate(pos_vel_);
        tSh->replicate(tension_);
        wrk->replicate(pos_vel_);

        sht_.forward(pos_vel_, *wrk, *voxSh);
        sht_.forward(tension_, *wrk, *tSh);

        COUTDEBUG("Copy initial guess to parallel solution array");
        voxSh->getDevice().Memcpy(i    , voxSh->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        tSh->getDevice().Memcpy(  i+vsz, tSh->begin()  , tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

        recycle(voxSh);
        recycle(tSh);
        recycle(wrk);
    }

    CHK(parallel_u_->RestoreArray(i));
    CHK(parallel_solver_->InitialGuessNonzero(true));
    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::ImplicitMatvecPhysical(Vec_t &vox, Sca_t &ten) const
{
    PROFILESTART();

    std::auto_ptr<Vec_t> f   = checkoutVec();
    std::auto_ptr<Vec_t> Sf  = checkoutVec();
    std::auto_ptr<Vec_t> Du  = checkoutVec();
    f->replicate(vox);
    Sf->replicate(vox);
    Du->replicate(vox);

    COUTDEBUG("Computing the interfacial forces and setting single-layer density");
    if (params_.solve_for_velocity){
        // Bending of dt*u + tension of sigma
        axpy(dt_, vox, *Du);
        Intfcl_force_.implicitTractionJump(S_, *Du, ten, *f);
    } else {
        // dt*(Bending of x + tension of sigma)
        Intfcl_force_.implicitTractionJump(S_, vox, ten, *f);
        axpy(dt_, *f, *f);
    }
    stokes_.SetDensitySL(f.get());

    if( ves_props_.has_contrast ){
        COUTDEBUG("Setting the double-layer density");
        av(ves_props_.dl_coeff, vox, *Du);
        stokes_.SetDensityDL(Du.get());
    } else {
        stokes_.SetDensityDL(NULL);
    }

    COUTDEBUG("Calling stokes");
    stokes_(*Sf);

    COUTDEBUG("Computing the div term");
    //! @note For some reason, doing the linear algebraic manipulation
    //! and writing the constraint as -\div{S[f_b+f_s]} = \div{u_inf
    //! almost halves the number of gmres iterations. Also having the
    //! minus sign in the matvec is tangibly better (1-2
    //! iterations). Need to investigate why.
    S_.div(*Sf, ten);
    axpy((value_type) -1.0, ten, ten);

    if( ves_props_.has_contrast )
        av(ves_props_.vel_coeff, vox, vox);

    axpy((value_type) -1.0, *Sf, vox, vox);

    ASSERT(vox.getDevice().isNumeric(vox.begin(), vox.size()), "Non-numeric velocity");
    ASSERT(ten.getDevice().isNumeric(ten.begin(), ten.size()), "Non-numeric divergence");

    recycle(f);
    recycle(Sf);
    recycle(Du);

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
ImplicitApply(const POp_t *o, const value_type *x, value_type *y)
{
    PROFILESTART();
    const InterfacialVelocity *F(NULL);
    o->Context((const void**) &F);
    size_t vsz(F->stokesBlockSize()), tsz(F->tensionBlockSize());

    std::auto_ptr<Vec_t> vox = F->checkoutVec();
    std::auto_ptr<Sca_t> ten = F->checkoutSca();
    vox->replicate(F->pos_vel_);
    ten->replicate(F->tension_);

    COUTDEBUG("Unpacking the input from parallel vector");
    if (F->params_.pseudospectral){
        vox->getDevice().Memcpy(vox->begin(), x    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
        ten->getDevice().Memcpy(ten->begin(), x+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    } else {  /* Galerkin */
        std::auto_ptr<Vec_t> voxSh = F->checkoutVec();
        std::auto_ptr<Sca_t> tSh   = F->checkoutSca();
        std::auto_ptr<Vec_t> wrk   = F->checkoutVec();

        voxSh->replicate(*vox);
        tSh->replicate(*ten);
        wrk->replicate(*vox);
        voxSh->getDevice().Memcpy(voxSh->begin(), x    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
        tSh  ->getDevice().Memcpy(tSh->begin()  , x+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);

        COUTDEBUG("Mapping the input to physical space");
        F->sht_.backward(*voxSh, *wrk, *vox);
        F->sht_.backward(*tSh  , *wrk, *ten);

        F->recycle(voxSh);
        F->recycle(tSh);
        F->recycle(wrk);
    }

    F->ImplicitMatvecPhysical(*vox, *ten);

    if (F->params_.pseudospectral){
        COUTDEBUG("Packing the matvec into parallel vector");
        vox->getDevice().Memcpy(y    , vox->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        ten->getDevice().Memcpy(y+vsz, ten->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    } else {  /* Galerkin */
        COUTDEBUG("Mapping the matvec to physical space");
        std::auto_ptr<Vec_t> voxSh = F->checkoutVec();
        std::auto_ptr<Sca_t> tSh   = F->checkoutSca();
        std::auto_ptr<Vec_t> wrk   = F->checkoutVec();

        voxSh->replicate(*vox);
        tSh->replicate(*ten);
        wrk->replicate(*vox);

        F->sht_.forward(*vox, *wrk, *voxSh);
        F->sht_.forward(*ten, *wrk, *tSh);

        COUTDEBUG("Packing the matvec into parallel vector");
        voxSh->getDevice().Memcpy(y    , voxSh->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        tSh  ->getDevice().Memcpy(y+vsz, tSh->begin()  , tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

        F->recycle(voxSh);
        F->recycle(tSh);
        F->recycle(wrk);
    }

    F->recycle(vox);
    F->recycle(ten);

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
ImplicitPrecond(const PSolver_t *ksp, const value_type *x, value_type *y)
{
    PROFILESTART();
    const InterfacialVelocity *F(NULL);
    ksp->PrecondContext((const void**) &F);

    size_t vsz(F->stokesBlockSize()), tsz(F->tensionBlockSize());

    std::auto_ptr<Vec_t> vox = F->checkoutVec();
    std::auto_ptr<Vec_t> vxs = F->checkoutVec();
    std::auto_ptr<Vec_t> wrk = F->checkoutVec();
    vox->replicate(F->pos_vel_);
    vxs->replicate(F->pos_vel_);
    wrk->replicate(F->pos_vel_);

    std::auto_ptr<Sca_t> ten = F->checkoutSca();
    std::auto_ptr<Sca_t> tns = F->checkoutSca();
    ten->replicate(F->tension_);
    tns->replicate(F->tension_);

    COUTDEBUG("Unpacking the input parallel vector");
    if (F->params_.pseudospectral){
        vox->getDevice().Memcpy(vox->begin(), x    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
        ten->getDevice().Memcpy(ten->begin(), x+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);
        F->sht_.forward(*vox, *wrk, *vxs);
        F->sht_.forward(*ten, *wrk, *tns);
    } else {  /* Galerkin */
        vxs->getDevice().Memcpy(vxs->begin(), x    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
        tns->getDevice().Memcpy(tns->begin(), x+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    }

    COUTDEBUG("Applying diagonal preconditioner");
    F->sht_.ScaleFreq(vxs->begin(), vxs->getNumSubFuncs(), F->position_precond.begin(), vxs->begin());
    F->sht_.ScaleFreq(tns->begin(), tns->getNumSubFuncs(), F->tension_precond.begin() , tns->begin());

    if (F->params_.pseudospectral){
        F->sht_.backward(*vxs, *wrk, *vox);
        F->sht_.backward(*tns, *wrk, *ten);
        vox->getDevice().Memcpy(y    , vox->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        ten->getDevice().Memcpy(y+vsz, ten->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    } else {  /* Galerkin */
        vxs->getDevice().Memcpy(y    , vxs->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        tns->getDevice().Memcpy(y+vsz, tns->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    }

    F->recycle(vox);
    F->recycle(vxs);
    F->recycle(wrk);
    F->recycle(ten);
    F->recycle(tns);

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
JacobiImplicitApply(const GMRESLinSolver<value_type> *o, const value_type *x, value_type *y)
{
    PROFILESTART();
    const InterfacialVelocity *F(NULL);
    o->Context((const void**) &F);
    size_t vsz(F->stokesBlockSize()), tsz(F->tensionBlockSize());

    std::auto_ptr<Vec_t> vox = F->checkoutVec();
    std::auto_ptr<Sca_t> ten = F->checkoutSca();
    vox->replicate(F->pos_vel_);
    ten->replicate(F->tension_);
    
    std::auto_ptr<Vec_t> vox_y = F->checkoutVec();
    std::auto_ptr<Sca_t> ten_y = F->checkoutSca();
    vox_y->replicate(F->pos_vel_);
    ten_y->replicate(F->tension_);

    vox->getDevice().Memcpy(vox->begin(), x    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    ten->getDevice().Memcpy(ten->begin(), x+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);

    F->operator()(*vox, *ten, *vox_y, *ten_y);

    vox_y->getDevice().Memcpy(y    , vox_y->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    ten_y->getDevice().Memcpy(y+vsz, ten_y->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

    F->recycle(vox);
    F->recycle(ten);
    
    F->recycle(vox_y);
    F->recycle(ten_y);

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
JacobiImplicitPrecond(const GMRESLinSolver<value_type> *o, const value_type *x, value_type *y)
{
    PROFILESTART();
    const InterfacialVelocity *F(NULL);
    o->Context((const void**) &F);
    
    size_t vsz(F->stokesBlockSize()), tsz(F->tensionBlockSize());
    
    Arr_t::getDevice().Memcpy(y, x, (vsz+tsz) * sizeof(value_type), device_type::MemcpyHostToHost);
    /*
    //Vec_t::getDevice().Memcpy(y, x, (vsz+tsz)*sizeof(value_type), device_type::MemcpyHostToHost);

    std::auto_ptr<Vec_t> vox = F->checkoutVec();
    std::auto_ptr<Vec_t> vxs = F->checkoutVec();
    std::auto_ptr<Vec_t> wrk = F->checkoutVec();
    vox->replicate(F->pos_vel_);
    vxs->replicate(F->pos_vel_);
    wrk->replicate(F->pos_vel_);

    std::auto_ptr<Sca_t> ten = F->checkoutSca();
    std::auto_ptr<Sca_t> tns = F->checkoutSca();
    ten->replicate(F->tension_);
    tns->replicate(F->tension_);

    COUTDEBUG("Unpacking the input parallel vector");
    vox->getDevice().Memcpy(vox->begin(), x    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    ten->getDevice().Memcpy(ten->begin(), x+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    //axpy(static_cast<value_type>(1.0),*vox,*vox);
    //axpy(static_cast<value_type>(1.0),*ten,*ten);
    
    //F->sht_.forward(*vox, *wrk, *vxs);
    //F->sht_.forward(*ten, *wrk, *tns);

    //COUTDEBUG("Applying diagonal preconditioner");
    //F->sht_.ScaleFreq(vxs->begin(), vxs->getNumSubFuncs(), F->position_precond.begin(), vxs->begin());
    //F->sht_.ScaleFreq(tns->begin(), tns->getNumSubFuncs(), F->tension_precond.begin() , tns->begin());

    //F->sht_.backward(*vxs, *wrk, *vox);
    //F->sht_.backward(*tns, *wrk, *ten);
    
    vox->getDevice().Memcpy(y    , vox->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    ten->getDevice().Memcpy(y+vsz, ten->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

    F->recycle(vox);
    F->recycle(vxs);
    F->recycle(wrk);
    F->recycle(ten);
    F->recycle(tns);
    */

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
JacobiImplicitLCPApply(const GMRESLinSolver<value_type> *o, const value_type *x, value_type *y)
{
    PROFILESTART();
    const InterfacialVelocity *F(NULL);
    o->Context((const void**) &F);
    size_t vsz(F->stokesBlockSize()), tsz(F->tensionBlockSize()), lsz(F->num_cvs_);

    std::auto_ptr<Vec_t> vox = F->checkoutVec();
    std::auto_ptr<Sca_t> ten = F->checkoutSca();
    vox->replicate(F->pos_vel_);
    ten->replicate(F->tension_);
    Arr_t lam(lsz);
    
    std::auto_ptr<Vec_t> vox_y = F->checkoutVec();
    std::auto_ptr<Sca_t> ten_y = F->checkoutSca();
    vox_y->replicate(F->pos_vel_);
    ten_y->replicate(F->tension_);
    Arr_t lam_y(lsz);

    vox->getDevice().Memcpy(vox->begin(), x    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    ten->getDevice().Memcpy(ten->begin(), x+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    lam.getDevice().Memcpy(lam.begin(), x+vsz+tsz, lsz * sizeof(value_type), device_type::MemcpyHostToDevice);

    F->operator()(*vox, *ten, lam, *vox_y, *ten_y, lam_y);

    vox_y->getDevice().Memcpy(y    , vox_y->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    ten_y->getDevice().Memcpy(y+vsz, ten_y->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    lam_y.getDevice().Memcpy(y+vsz+tsz, lam_y.begin(), lsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

    F->recycle(vox);
    F->recycle(ten);
    
    F->recycle(vox_y);
    F->recycle(ten_y);

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
JacobiImplicitLCPPrecond(const GMRESLinSolver<value_type> *o, const value_type *x, value_type *y)
{
    PROFILESTART();
    const InterfacialVelocity *F(NULL);
    o->Context((const void**) &F);
    
    size_t vsz(F->stokesBlockSize()), tsz(F->tensionBlockSize()), lsz(F->num_cvs_);

    Arr_t::getDevice().Memcpy(y, x, (vsz+tsz+lsz) * sizeof(value_type), device_type::MemcpyHostToHost);
    
    /* 
    std::auto_ptr<Vec_t> vox = F->checkoutVec();
    std::auto_ptr<Vec_t> vxs = F->checkoutVec();
    std::auto_ptr<Vec_t> wrk = F->checkoutVec();
    vox->replicate(F->pos_vel_);
    vxs->replicate(F->pos_vel_);
    wrk->replicate(F->pos_vel_);
    Arr_t lam(lsz);

    std::auto_ptr<Sca_t> ten = F->checkoutSca();
    std::auto_ptr<Sca_t> tns = F->checkoutSca();
    ten->replicate(F->tension_);
    tns->replicate(F->tension_);

    COUTDEBUG("Unpacking the input parallel vector");
    vox->getDevice().Memcpy(vox->begin(), x    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    ten->getDevice().Memcpy(ten->begin(), x+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    lam.getDevice().Memcpy(lam.begin(), x+vsz+tsz, lsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    
    axpy(static_cast<value_type>(1.0),*vox,*vox);
    axpy(static_cast<value_type>(1.0),*ten,*ten);
    
    F->sht_.forward(*vox, *wrk, *vxs);
    F->sht_.forward(*ten, *wrk, *tns);

    COUTDEBUG("Applying diagonal preconditioner");
    F->sht_.ScaleFreq(vxs->begin(), vxs->getNumSubFuncs(), F->position_precond.begin(), vxs->begin());
    F->sht_.ScaleFreq(tns->begin(), tns->getNumSubFuncs(), F->tension_precond.begin() , tns->begin());

    F->sht_.backward(*vxs, *wrk, *vox);
    F->sht_.backward(*tns, *wrk, *ten);
    
    vox->getDevice().Memcpy(y    , vox->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    ten->getDevice().Memcpy(y+vsz, ten->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    lam.getDevice().Memcpy(y+vsz+tsz, lam.begin(), lsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

    F->recycle(vox);
    F->recycle(vxs);
    F->recycle(wrk);

    F->recycle(ten);
    F->recycle(tns);
    */

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
Solve(const PVec_t *rhs, PVec_t *u0, const value_type &dt, const SolverScheme &scheme) const
{
    PROFILESTART();
    INFO("Solving for position/velocity and tension using "<<scheme<<" scheme.");

    Error_t err = parallel_solver_->Solve(parallel_rhs_, parallel_u_);
    typename PVec_t::size_type iter;
    CHK(parallel_solver_->IterationNumber(iter));

    INFO("Parallel solver returned after "<<iter<<" iteration(s).");
    parallel_solver_->ViewReport();

    PROFILEEND("",0);
    return err;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::Update(PVec_t *u0)
{
    PROFILESTART();
    COUTDEBUG("Updating position and tension.");
    size_t vsz(stokesBlockSize()), tsz(tensionBlockSize());

    typename PVec_t::iterator i(NULL);
    typename PVec_t::size_type rsz;

    CHK(u0->GetArray(i, rsz));
    ASSERT(rsz==vsz+tsz,"Bad sizes");

    if (params_.pseudospectral){
        COUTDEBUG("Copy data from parallel solution array");
        pos_vel_.getDevice().Memcpy(pos_vel_.begin(), i    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
        tension_.getDevice().Memcpy(tension_.begin(), i+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);
    } else { /* Galerkin */
        COUTDEBUG("Unpacking the solution from parallel vector");
        std::auto_ptr<Vec_t> voxSh = checkoutVec();
        std::auto_ptr<Sca_t> tSh   = checkoutSca();
        std::auto_ptr<Vec_t> wrk   = checkoutVec();

        voxSh->replicate(pos_vel_);
        tSh->replicate(tension_);
        wrk->replicate(pos_vel_);

        voxSh->getDevice().Memcpy(voxSh->begin(), i    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
        tSh  ->getDevice().Memcpy(tSh  ->begin(), i+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);

        COUTDEBUG("Mapping the solution to physical space");
        sht_.backward(*voxSh, *wrk, pos_vel_);
        sht_.backward(*tSh  , *wrk, tension_);

        recycle(voxSh);
        recycle(tSh);
        recycle(wrk);
    }

    CHK(u0->RestoreArray(i));

    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
BgFlow(Vec_t &bg, const value_type &dt) const{
    //!@bug the time should be passed to the BgFlow handle.
    bg_flow_(S_.getPosition(), 0, bg);

    return ErrorEvent::Success;
}

// Compute velocity_far = velocity_bg + FMM(bending+tension) - DirectStokes(bending+tension)
template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
updateFarField() const
{
    PROFILESTART();
    ASSERT(pos_vel_.size() == S_.getPosition().size(), "inccorrect size");
    //pos_vel_.replicate(S_.getPosition());
    //CHK(this->BgFlow(pos_vel_, this->dt_));
    
    if (this->interaction_.HasInteraction()){
        std::auto_ptr<Vec_t>        fi  = checkoutVec();
        std::auto_ptr<Vec_t>        vel = checkoutVec();
        fi->replicate(pos_vel_);
        vel->replicate(pos_vel_);

        Intfcl_force_.bendingForce(S_, *fi);
        Intfcl_force_.tensileForce(S_, tension_, *vel);
        axpy(static_cast<value_type>(1.0), *fi, *vel, *fi);
        
        // add contact force
        axpy(static_cast<value_type>(1.0), *fi, S_.fc_, *fi);

        // TODO: add gravity force

        //stokes_.SetTrgCoord(NULL);
        //stokes_.SetSrcCoord(S_.getPosition());
        stokes_.SetDensitySL(fi.get());
        if( ves_props_.has_contrast ){
            av(ves_props_.dl_coeff, pos_vel_, pos_vel_);
            stokes_.SetDensityDL(&pos_vel_);
        }
        else{
            stokes_.SetDensityDL(NULL);
        }
        stokes_.FarInteraction(*vel);
        
        /* Deprecated far-field interaction with SLP only
        //EvaluateFarInteraction(S_.getPosition(), *fi, *vel);
        */
               
        CHK(this->BgFlow(pos_vel_, this->dt_));
        axpy(static_cast<value_type>(1.0), *vel, pos_vel_, pos_vel_);

        recycle(fi);
        recycle(vel);
    }
    else{
        CHK(this->BgFlow(pos_vel_, this->dt_));
    }
    PROFILEEND("",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
CallInteraction(const Vec_t &src, const Vec_t &den, Vec_t &pot) const
{
    PROFILESTART();
    std::auto_ptr<Vec_t>        X = checkoutVec();
    std::auto_ptr<Vec_t>        D = checkoutVec();
    std::auto_ptr<Vec_t>        P = checkoutVec();

    X->replicate(src);
    D->replicate(den);
    P->replicate(pot);

    // shuffle
    ShufflePoints(src, *X);
    ShufflePoints(den, *D);
    P->setPointOrder(PointMajor);

    // far interactions
    Error_t status;
    CHK( status = interaction_(*X, *D, *P));

    // shuffle back
    ShufflePoints(*P, pot);

    X->setPointOrder(AxisMajor);        /* ignoring current content */
    D->setPointOrder(AxisMajor);        /* ignoring current content */
    P->setPointOrder(AxisMajor);        /* ignoring current content */

    recycle(X);
    recycle(D);
    recycle(P);
    PROFILEEND("",0);

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
EvaluateFarInteraction(const Vec_t &src, const Vec_t &fi, Vec_t &vel) const
{
    if ( params_.interaction_upsample ){
        return EvalFarInter_ImpUpsample(src, fi, vel);
    } else {
        return EvalFarInter_Imp(src, fi, vel);
    }
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
EvalFarInter_Imp(const Vec_t &src, const Vec_t &fi, Vec_t &vel) const
{
    std::auto_ptr<Vec_t> den    = checkoutVec();
    std::auto_ptr<Vec_t> slf    = checkoutVec();

    den->replicate(src);
    slf->replicate(vel);

    // multiply area elment into density
    xv(S_.getAreaElement(), fi, *den);

    // compute self (to subtract)
    slf->getDevice().DirectStokes(src.begin(), den->begin(), quad_weights_.begin(),
        slf->getStride(), slf->getStride(), slf->getNumSubs(), src.begin() /* target */,
        0, slf->getStride() /* number of trgs per surface */,
        slf->begin());

    // incorporating the quadrature weights into the density (use pos as temp holder)
    ax<Sca_t>(quad_weights_, *den, *den);

    CHK(CallInteraction(src, *den, vel));

    // subtract self
    axpy(static_cast<value_type>(-1.0), *slf, vel, vel);

    recycle(den);
    recycle(slf);

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
EvalFarInter_ImpUpsample(const Vec_t &src, const Vec_t &fi, Vec_t &vel) const
{
    std::auto_ptr<Vec_t> pos = checkoutVec();
    std::auto_ptr<Vec_t> den = checkoutVec();
    std::auto_ptr<Vec_t> pot = checkoutVec();
    std::auto_ptr<Vec_t> slf = checkoutVec();
    std::auto_ptr<Vec_t> shc = checkoutVec();
    std::auto_ptr<Vec_t> wrk = checkoutVec();

    // prepare for upsampling
    int usf(sht_upsample_.getShOrder());
    pos->resize(src.getNumSubs(), usf);
    den->resize(src.getNumSubs(), usf);
    slf->resize(src.getNumSubs(), usf);
    shc->resize(src.getNumSubs(), usf);
    wrk->resize(src.getNumSubs(), usf);

    // multiply area elment into density (using pot as temp)
    pot->replicate(fi);
    xv(S_.getAreaElement(), fi, *pot);

    // upsample position and density
    Resample( src, sht_, sht_upsample_, *shc, *wrk, *pos);
    Resample(*pot, sht_, sht_upsample_, *shc, *wrk, *den);
    pot->resize(src.getNumSubs(), usf);

    // compute self (to subtract)
    slf->getDevice().DirectStokes(pos->begin(), den->begin(), quad_weights_up_.begin(),
        slf->getStride(), slf->getStride(), slf->getNumSubs(), pos->begin() /* target */,
        0, slf->getStride() /* number of trgs per surface */,
        slf->begin());

    // incorporating the quadrature weights into the density
    ax<Sca_t>(quad_weights_up_, *den, *den);

    CHK(CallInteraction(*pos, *den, *pot));

    // subtract self
    axpy(static_cast<value_type>(-1.0), *slf, *pot, *slf);

    // downsample
    Resample(*slf, sht_upsample_, sht_, *shc, *wrk, *pot);
    sht_.lowPassFilter(*pot, *wrk, *shc, vel);

    recycle(pos);
    recycle(den);
    recycle(pot);
    recycle(slf);
    recycle(shc);
    recycle(wrk);

    return ErrorEvent::Success;
}

// Linear solve to compute tension such that surface divergence:
// surf_div( velocity + stokes(tension) ) = 0
template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::getTension(
    const Vec_t &vel_in, Sca_t &tension) const
{
    PROFILESTART();
    std::auto_ptr<Sca_t> rhs = checkoutSca();
    std::auto_ptr<Sca_t> wrk = checkoutSca();

    S_.div(vel_in, *rhs);

    //! this just negates rhs (not a bug; bad naming for overleaded axpy)
    axpy(static_cast<value_type>(-1), *rhs, *rhs);

    int iter(params_.time_iter_max);
    int rsrt(params_.time_iter_max);
    value_type tol(params_.time_tol),relres(params_.time_tol);
    enum BiCGSReturn solver_ret;
    Error_t ret_val(ErrorEvent::Success);

    COUTDEBUG("Solving for tension");
    solver_ret = linear_solver_(*this, tension, *rhs, rsrt, iter, relres);

    if ( solver_ret  != BiCGSSuccess )
        ret_val = ErrorEvent::DivergenceError;

    COUTDEBUG("Tension solve: Total iter = "<< iter<<", relres = "<<relres);
    COUTDEBUG("Checking true relres");
    ASSERT(((*this)(tension, *wrk),
            axpy(static_cast<value_type>(-1), *wrk, *rhs, *wrk),
            relres = sqrt(AlgebraicDot(*wrk, *wrk))/sqrt(AlgebraicDot(*rhs,*rhs)),
            relres<tol
            ),
           "relres ("<<relres<<")<tol("<<tol<<")"
           );

    recycle(wrk);
    recycle(rhs);
    PROFILEEND("",0);

    return ret_val;
}

// Computes near (self) velocity due to force.
// Computes singular integration on the vesicle surface.
template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::stokes(
    const Vec_t &force, Vec_t &velocity) const
{
    PROFILESTART();

    /*
    int imax(S_.getPosition().getGridDim().first);
    int jmax(S_.getPosition().getGridDim().second);
    int np = S_.getPosition().getStride();
    int nv = S_.getPosition().getNumSubs();

    std::auto_ptr<Sca_t> t1 = checkoutSca();
    std::auto_ptr<Sca_t> t2 = checkoutSca();
    std::auto_ptr<Vec_t> v1 = checkoutVec();
    std::auto_ptr<Vec_t> v2 = checkoutVec();

    ax(w_sph_inv_, S_.getAreaElement(), *t1);

    int numinputs = 3;
    const Sca_t* inputs[] = {&S_.getPosition(), &force, t1.get()};
    Sca_t* outputs[] = {v1.get(), v2.get(), t2.get()};

    for(int ii=0;ii < imax; ++ii)
        for(int jj=0;jj < jmax; ++jj)
        {
            //move_pole(ii, jj, outputs);
            assert(false); exit(1); //@bug: move_pole deprecated

            ax(w_sph_, *t2, *t2);
            xv(*t2, *v2, *v2);

            S_.getPosition().getDevice().DirectStokes(v1->begin(), v2->begin(),
                sing_quad_weights_.begin(), np, np, nv, S_.getPosition().begin(),
                ii * jmax + jj, ii * jmax + jj + 1, velocity.begin());
        }

    recycle(t1);
    recycle(t2);
    recycle(v1);
    recycle(v2);
    */

    velocity.replicate(force);
    stokes_.SetDensitySL(&force);
    stokes_.SetDensityDL(NULL);
    stokes_.SelfInteraction(velocity);

    PROFILEEND("SelfInteraction_",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::stokes_double_layer(
    const Vec_t &force, Vec_t &velocity) const
{
    PROFILESTART();

    int imax(S_.getPosition().getGridDim().first);
    int jmax(S_.getPosition().getGridDim().second);
    int np = S_.getPosition().getStride();
    int nv = S_.getPosition().getNumSubs();

    std::auto_ptr<Sca_t> t1 = checkoutSca();
    std::auto_ptr<Sca_t> t2 = checkoutSca();
    std::auto_ptr<Vec_t> v1 = checkoutVec();
    std::auto_ptr<Vec_t> v2 = checkoutVec();
    std::auto_ptr<Vec_t> v3 = checkoutVec();

    ax(w_sph_inv_, S_.getAreaElement(), *t1);

    int numinputs = 4;
    const Sca_t* inputs[] = {&S_.getPosition(), &S_.getNormal(),   &force, t1.get()};
    Sca_t*      outputs[] = { v1.get()        ,  v3.get()      , v2.get(), t2.get()};

    for(int ii=0;ii < imax; ++ii)
        for(int jj=0;jj < jmax; ++jj)
        {
            //move_pole(ii, jj, outputs);
            assert(false); exit(1); //@bug: move_pole deprecated

            ax(w_sph_, *t2, *t2);
            xv(*t2, *v2, *v2);

            S_.getPosition().getDevice().DirectStokesDoubleLayer(v1->begin(), v3->begin(), v2->begin(),
                sing_quad_weights_.begin(), np, nv, S_.getPosition().begin(),
                ii * jmax + jj, ii * jmax + jj + 1, velocity.begin());

        }

    recycle(t1);
    recycle(t2);
    recycle(v1);
    recycle(v2);

    PROFILEEND("DblLayerSelfInteraction_",0);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
operator()(const Vec_t &x_new, Vec_t &time_mat_vec) const
{
    PROFILESTART();
    std::auto_ptr<Vec_t> fb = checkoutVec();

    COUTDEBUG("Time matvec");
    Intfcl_force_.linearBendingForce(S_, x_new, *fb);
    CHK(stokes(*fb, time_mat_vec));
    axpy(-dt_, time_mat_vec, x_new, time_mat_vec);
    recycle(fb);
    PROFILEEND("",0);

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
operator()(const Sca_t &tension, Sca_t &div_stokes_fs) const
{
    std::auto_ptr<Vec_t> fs = checkoutVec();
    std::auto_ptr<Vec_t> u = checkoutVec();

    COUTDEBUG("Tension matvec");
    Intfcl_force_.tensileForce(S_, tension, *fs);
    CHK(stokes(*fs, *u));
    S_.div(*u, div_stokes_fs);

    recycle(fs);
    recycle(u);

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
operator()(const Vec_t &x_new, const Sca_t &tension, Vec_t &time_mat_vec, Sca_t &div_stokes_fs) const
{
    std::auto_ptr<Vec_t> f = checkoutVec();
    std::auto_ptr<Vec_t> dlp_tmp = checkoutVec();
    f->replicate(x_new);
    dlp_tmp->replicate(x_new);
 
    axpy(dt_, x_new, *dlp_tmp);
    Intfcl_force_.implicitTractionJump(S_, *dlp_tmp, tension, *f);
    CHK(stokes(*f, time_mat_vec));

    if (ves_props_.has_contrast){
        av(ves_props_.dl_coeff, x_new, *f);
        
        stokes_.SetDensitySL(NULL);
        stokes_.SetDensityDL(f.get());
        stokes_.SelfInteraction(*dlp_tmp);
        
        axpy(static_cast<value_type>(1.0), *dlp_tmp, time_mat_vec, time_mat_vec);
    }
    
    S_.div(time_mat_vec, div_stokes_fs);
    
    if (ves_props_.has_contrast){
        av(ves_props_.vel_coeff, x_new, *f);
        axpy(static_cast<value_type>(-1.0), time_mat_vec, *f, time_mat_vec);
    }
    else{
        axpy(static_cast<value_type>(-1.0), time_mat_vec, x_new, time_mat_vec);
    }
    
    recycle(f);
    recycle(dlp_tmp);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
operator()(const Vec_t &x_new, const Sca_t &tension, const Arr_t &lambda,
        Vec_t &time_mat_vec, Sca_t &tension_mat_vec, Arr_t &lambda_mat_vec) const
{
    std::auto_ptr<Vec_t> f_col = checkoutVec();
    std::auto_ptr<Vec_t> vel_f_col = checkoutVec();
    std::auto_ptr<Sca_t> div_vel_f_col = checkoutSca();
    
    f_col->replicate(x_new);
    vel_f_col->replicate(x_new);
    div_vel_f_col->replicate(tension);

    (*this)(x_new, tension, time_mat_vec, tension_mat_vec);

    CVJacobianTrans(lambda, *f_col);
    stokes(*f_col, *vel_f_col);
    
    axpy(static_cast<value_type>(-1.0), *vel_f_col, time_mat_vec, time_mat_vec);
    
    S_.div(*vel_f_col, *div_vel_f_col);
    axpy(static_cast<value_type>(1.0), *div_vel_f_col, tension_mat_vec, tension_mat_vec);

    axpy(dt_, x_new, *f_col);
    CVJacobian(*f_col, lambda_mat_vec);
    LCPSelect(lambda, lambda_mat_vec);
    
    recycle(f_col);
    recycle(vel_f_col);
    recycle(div_vel_f_col);

    return ErrorEvent::Success;
}

// This function evaluates J*dx, where J is the contact volumes' Jacobian,
// dx is the displacement stored in x_new.
// J is sparse matrix of size ncv*N, where ncv is the number of contact
// volumes, N is the total points on vesicles.
// The ith row of J is the contact volume gradient of ith contact volume.
// Instead of forming J, we use vGrad_index which stores the components
// index(belongs to which contact volume) to evaluate J*dx.
// The result dx_matvec=J*dx is of size ncv*1, the ith components of 
// dx_matvec is the sum of all the components of xy(x_new,vGrad) which have
// index i(stored in vGrad_index).
// Since we are accessing all the components of the Vec_t, we copy the vector
// from device to host.
template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
CVJacobian(const Vec_t &x_new, Arr_t &lambda_mat_vec) const
{
    std::auto_ptr<Vec_t> x_vgrad = checkoutVec();
    x_vgrad->replicate(x_new);

    xy(x_new, vgrad_, *x_vgrad);

    //axpy(dt_, *x_vgrad, *x_vgrad);
    
    std::vector<value_type> x_vgrad_host;
    x_vgrad_host.resize(x_new.size(), 0.0);
    // copy x_vgrad to host
    x_vgrad->getDevice().Memcpy(&x_vgrad_host.front(), x_vgrad->begin(),
        x_vgrad->size() * sizeof(value_type),
        x_vgrad->getDevice().MemcpyDeviceToHost);

    std::vector<value_type> lambda_mat_vec_host(lambda_mat_vec.size(), 0.0);

    size_t ncount = vgrad_.size();
//#pragma omp parallel for
    for (size_t icount = 0; icount < ncount; icount++)
    {
        if(vgrad_ind_[icount] > 0)
        {
            lambda_mat_vec_host[ vgrad_ind_[icount] - 1 ] += x_vgrad_host[icount];
//#pragma omp atomic
//            {
//            }
        }
    }

    // copy lambda_mat_vec to device
    lambda_mat_vec.getDevice().Memcpy(lambda_mat_vec.begin(), &lambda_mat_vec_host.front(),
            lambda_mat_vec.size() * sizeof(value_type),
            lambda_mat_vec.getDevice().MemcpyHostToDevice);

    recycle(x_vgrad);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
CVJacobianTrans(const Arr_t &lambda, Vec_t &f_col) const
{
    std::vector<value_type> f_col_host;
    f_col_host.resize(vgrad_.size(), 0.0);

    std::vector<value_type> vgrad_host;
    vgrad_host.resize(vgrad_.size(), 0.0);
    // copy vgrad_ to host
    vgrad_.getDevice().Memcpy(&vgrad_host.front(), vgrad_.begin(),
        vgrad_.size() * sizeof(value_type),
        vgrad_.getDevice().MemcpyDeviceToHost);

    std::vector<value_type> lambda_host;
    lambda_host.resize(lambda.size(), 0.0);
    // copy lambda to host
    lambda.getDevice().Memcpy(&lambda_host.front(), lambda.begin(),
            lambda.size() * sizeof(value_type),
            lambda.getDevice().MemcpyDeviceToHost);

    size_t ncount = vgrad_.size();
#pragma omp parallel for
    for (size_t icount = 0; icount < ncount; icount++)
    {
        if(vgrad_ind_[icount] > 0)
            f_col_host[icount] = lambda_host[ vgrad_ind_[icount] - 1 ] * vgrad_host[icount];
    }
    
    /*
    f_col.getDevice().Memcpy(f_col.begin(), &f_col_host.front(),
        f_col.size() * sizeof(value_type),
        f_col.getDevice().MemcpyHostToDevice);
    */
        
    //filtered f_col
    std::auto_ptr<Vec_t> xwrk = checkoutVec();
    xwrk->replicate(S_.getPosition());
    xwrk->getDevice().Memcpy(xwrk->begin(), &f_col_host.front(),
        xwrk->size() * sizeof(value_type),
        xwrk->getDevice().MemcpyHostToDevice);

    sht_filter_high(*xwrk, f_col, &sht_, params_.rep_exponent);

    recycle(xwrk);
    //end of filtered

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
LCPSelect(const Arr_t &lambda, Arr_t &lambda_mat_vec) const
{
    std::vector<value_type> lambda_host;
    std::vector<value_type> lambda_mat_vec_host;

    lambda_host.resize(lambda.size(), 0.0);
    lambda_mat_vec_host.resize(lambda.size(), 0.0);

    lambda.getDevice().Memcpy(&lambda_host.front(), lambda.begin(),
            lambda.size() * sizeof(value_type),
            lambda.getDevice().MemcpyDeviceToHost);
    lambda_mat_vec.getDevice().Memcpy(&lambda_mat_vec_host.front(), lambda_mat_vec.begin(),
            lambda_mat_vec.size() * sizeof(value_type),
            lambda_mat_vec.getDevice().MemcpyDeviceToHost);

    size_t ncount = lambda.size();
//#pragma omp parallel for
    for (size_t icount = 0; icount < ncount; icount++)
    {
        if(PA_[icount] == 0)
            lambda_mat_vec_host[icount] = lambda_host[icount];
    
    }

    lambda_mat_vec.getDevice().Memcpy(lambda_mat_vec.begin(), &lambda_mat_vec_host.front(), 
            lambda_mat_vec.size() * sizeof(value_type),
            lambda_mat_vec.getDevice().MemcpyHostToDevice);

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
SolveLCP(Vec_t &u_lcp, Sca_t &ten_lcp, Arr_t &lambda_lcp, Arr_t &cvs) const
{
    /*
     * LCP_flag = 1, preprocessing
     * LCP_flag = 2, iterating
     * LCP_flag = 3, relative error termination
     * LCP_flag = 4, absolute error termination
     * LCP_flag = 5, stagnation
     * LCP_flag = 6, local minima
     * LCP_flag = 7, nondescent
     * LCP_flag = 8, maxlimit iters
     */

    int LCP_flag = 1;
    
    int LCP_n = num_cvs_;
    int LCP_max_iter = 100;

    // LCP parameters
    value_type LCP_eps = 1e-16;
    value_type LCP_h = 1e-7;
    value_type LCP_alpha = 0.5;
    value_type LCP_beta = 0.001;
    value_type LCP_gamma = 1e-28;
    value_type LCP_rho = LCP_eps;

    // setup
    std::vector<value_type> LCP_convergence(LCP_max_iter, 0.0);
    value_type LCP_err = 1e+16;
    int LCP_iter = 1;

    // checkout vecs, scas and arrs for calculation
    std::auto_ptr<Vec_t> time_mat_vec = checkoutVec();
    std::auto_ptr<Vec_t> du = checkoutVec();
    time_mat_vec->replicate(u_lcp);
    du->replicate(u_lcp);

    std::auto_ptr<Sca_t> tension_mat_vec = checkoutSca();
    std::auto_ptr<Sca_t> dtension = checkoutSca();
    tension_mat_vec->replicate(ten_lcp);
    dtension->replicate(ten_lcp);
    
    Arr_t lambda_mat_vec(LCP_n);
    Arr_t dlambda(LCP_n);
    Arr_t LCP_y(LCP_n);
    Arr_t LCP_phi(LCP_n);
    
    // init vecs, scas and arrs
    axpy(static_cast<value_type>(0.0), u_lcp, u_lcp);
    axpy(static_cast<value_type>(0.0), *time_mat_vec, *time_mat_vec);
    axpy(static_cast<value_type>(0.0), *du, *du);

    axpy(static_cast<value_type>(0.0), ten_lcp, ten_lcp);
    axpy(static_cast<value_type>(0.0), *tension_mat_vec, *tension_mat_vec);
    axpy(static_cast<value_type>(0.0), *dtension, *dtension);

    Arr_t::getDevice().axpy(static_cast<value_type>(0.0), lambda_lcp.begin(), static_cast<value_type*>(NULL), 
                lambda_lcp.size(), lambda_lcp.begin());
    Arr_t::getDevice().axpy(static_cast<value_type>(0.0), lambda_mat_vec.begin(), static_cast<value_type*>(NULL), 
                lambda_mat_vec.size(), lambda_mat_vec.begin());
    Arr_t::getDevice().axpy(static_cast<value_type>(0.0), dlambda.begin(), static_cast<value_type*>(NULL), 
                dlambda.size(), dlambda.begin());
    Arr_t::getDevice().axpy(static_cast<value_type>(0.0), LCP_y.begin(), static_cast<value_type*>(NULL), 
                LCP_y.size(), LCP_y.begin());
    Arr_t::getDevice().axpy(static_cast<value_type>(0.0), LCP_phi.begin(), static_cast<value_type*>(NULL), 
                LCP_phi.size(), LCP_phi.begin());
    
    // allocate memory for solving Newton's system
    size_t vsz(stokesBlockSize()), tsz(tensionBlockSize()), lsz(num_cvs_);
    ASSERT(S_.getPosition().size()==vsz,"Bad sizes");
    size_t N_size = vsz+tsz+lsz;
    value_type x_host[N_size], rhs_host[N_size];
    
    int iter(params_.time_iter_max);
    value_type relres(params_.time_tol);
    
    value_type LCP_old_err;
    LCP_flag = 2;
    while(LCP_iter < LCP_max_iter)
    {
        // if size of PA is large, should we not resize every iteration
        PA_.clear();
        PA_.resize(num_cvs_, 1);
        (*this)(u_lcp, ten_lcp, lambda_lcp, *time_mat_vec, *tension_mat_vec, lambda_mat_vec);
        
        Arr_t::getDevice().axpy(static_cast<value_type>(1.0), lambda_mat_vec.begin(), cvs.begin(), 
                num_cvs_, LCP_y.begin());
        
        minmap(LCP_y, lambda_lcp, LCP_phi);
        
        LCP_old_err = LCP_err;
        LCP_err = 0.5 * Arr_t::getDevice().AlgebraicDot(LCP_phi.begin(), LCP_phi.begin(), LCP_phi.size());

        INFO("LCP Newtown iter: "<<LCP_iter<<". -- err: "<<LCP_err<<" -- relative err: "
                <<fabs(LCP_err - LCP_old_err)/fabs(LCP_old_err) );

        INFO("lambda: "<<lambda_lcp);

        // relative stopping criteria
        if(fabs(LCP_err - LCP_old_err)/fabs(LCP_old_err) < 1e-6)
        {
            LCP_flag = 3;
            break;
        }
        
        // absolute stopping criteria
        if(LCP_err < 1e-21)
        {
            LCP_flag =4;
            break;
        }

        // solve the Newton system to get descent direction
        // copy device type to value_type array to call GMRES
        axpy(static_cast<value_type>(0.0), *du, *du);
        axpy(static_cast<value_type>(0.0), *dtension, *dtension);
        Arr_t::getDevice().axpy(static_cast<value_type>(0.0), dlambda.begin(), static_cast<value_type*>(NULL), 
                dlambda.size(), dlambda.begin());
        Arr_t::getDevice().axpy(static_cast<value_type>(-1.0), LCP_phi.begin(), static_cast<value_type*>(NULL), 
                LCP_phi.size(), LCP_phi.begin());

        // copy to unknown solution
        du->getDevice().Memcpy(x_host, du->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        dtension->getDevice().Memcpy(x_host+vsz, dtension->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        dlambda.getDevice().Memcpy(x_host+vsz+tsz, dlambda.begin(), lsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
    
        // copy to rhs
        du->getDevice().Memcpy(rhs_host, du->begin(), vsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        dtension->getDevice().Memcpy(rhs_host+vsz, dtension->begin(), tsz * sizeof(value_type), device_type::MemcpyDeviceToHost);
        LCP_phi.getDevice().Memcpy(rhs_host+vsz+tsz, LCP_phi.begin(), lsz * sizeof(value_type), device_type::MemcpyDeviceToHost);

        // solve the linear system using gmres
        // set relative tolerance
        value_type LCP_lin_tol = 1.0e-03;
        //if( MaxAbs(LCP_phi) < 1.0e-05 )
        //    LCP_lin_tol = 5.0e-01;
        
        INFO("Begin of SolveLCP Newton system.");
        int solver_ret = linear_solver_gmres_(JacobiImplicitLCPApply, JacobiImplicitLCPPrecond, x_host, rhs_host, 
            LCP_lin_tol, 8.0e-11, N_size, iter, 300);
        INFO("End of SolveLCP Newton system.");

        // copy host to device
        du->getDevice().Memcpy(du->begin(), x_host    , vsz * sizeof(value_type), device_type::MemcpyHostToDevice);
        dtension->getDevice().Memcpy(dtension->begin(), x_host+vsz, tsz * sizeof(value_type), device_type::MemcpyHostToDevice);
        dlambda.getDevice().Memcpy(dlambda.begin(), x_host+vsz+tsz, lsz * sizeof(value_type), device_type::MemcpyHostToDevice);

        // TODO: the gradient of meric function \nabla\theta is LCP_phi^T*LCP_matrix, 
        // which we can't get with matrix free version LCP solver, either form LCP matrix
        // explicitly or do some approximate \nabla\theta calculation.
        // So LCP_flag 6,7 is not tested, and we use Newton's method withou line search for now.
        // (Line search requires \nabla\theta)

        value_type dlambda_norm = Arr_t::getDevice().AlgebraicDot(dlambda.begin(), dlambda.begin(), dlambda.size());
        if(dlambda_norm < LCP_eps)
        {
            LCP_flag = 5;
            break;
            // could use dlambda = -nabla_theta
        }
        
        // TODO: test for whether dropped into a local minima flag 6
        // TODO: test for sufficient descent direction flag 7

        // update solution with direction calculated
        // TODO: do line search with \nabla\theta for acceptable LCP_tau
        value_type LCP_tau = 1.0;

        axpy(LCP_tau, *du, u_lcp, u_lcp);
        axpy(LCP_tau, *dtension, ten_lcp, ten_lcp);
        Arr_t::getDevice().axpy(LCP_tau, dlambda.begin(), lambda_lcp.begin(), 
                dlambda.size(), lambda_lcp.begin());
        
        //bool lambdaupdated = false;
        //check_lambda();
        // if lambdaupdated == true, solve for du and dtension of new lambda
        // end of update solution
    }

    recycle(time_mat_vec);
    recycle(du);
    recycle(tension_mat_vec);
    recycle(dtension);

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
minmap(const Arr_t &xin1, const Arr_t &xin2, Arr_t &xout) const
{
    const value_type* x1i = xin1.begin();
    const value_type* x2i = xin2.begin();
    value_type* xoi = xout.begin();
    
    size_t length = xin1.size();

#pragma omp parallel for
    for (size_t idx = 0; idx < length; idx++)
    {
        xoi[idx] = (x1i[idx] < x2i[idx]) ? x1i[idx] : x2i[idx];
        PA_[idx] = (x1i[idx] < x2i[idx]) ? 1 : 0;
    }

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
projectU1(Vec_t &u1, const Vec_t &x_old) const
{
    std::auto_ptr<Vec_t> xwrk = checkoutVec();
    xwrk->replicate(S_.getPosition());
    
    std::auto_ptr<Sca_t> twrk = checkoutSca();
    twrk->replicate(tension_);

    // pos_s stores the start configuration
    // pos_e stores the end configuration
    // vGrad stores the gradient of contact volumes
    std::vector<value_type> pos_s;
    std::vector<value_type> pos_e;
    std::vector<value_type> vGrad;
    pos_s.resize(S_.getPosition().size(),0.0);
    pos_e.resize(S_.getPosition().size(),0.0);
    vGrad.resize(S_.getPosition().size(),0.0);
    
    // copy current position to start configuration
    x_old.getDevice().Memcpy(&pos_s.front(), x_old.begin(),
            x_old.size() * sizeof(value_type),
            x_old.getDevice().MemcpyDeviceToHost);
 
    // the candidate position and copy to end configuration
    axpy(static_cast<value_type> (1.0), u1, x_old, *xwrk);
    xwrk->getDevice().Memcpy(&pos_e.front(), xwrk->begin(),
            xwrk->size() * sizeof(value_type),
            xwrk->getDevice().MemcpyDeviceToHost);

    int resolveCount = 0;
    std::vector<value_type> IV;
    if(params_.periodic_length > 0)
    {
        TransferVesicle(pos_s, pos_e);
    }
    CI_.getVolumeAndGradient(IV, num_cvs_, vGrad, vgrad_ind_, pos_s, pos_e, params_.min_sep_dist, params_.periodic_length);
    while(num_cvs_ > 0)
    {
        // worker for array size of num_cvs_
        Arr_t awrk(num_cvs_);
        
        // cvs stores the contact volumes
        Arr_t cvs(num_cvs_);
        // copy contact volumes to cvs
        cvs.getDevice().Memcpy(cvs.begin(), &IV.front(), 
                num_cvs_ * sizeof(value_type),
                cvs.getDevice().MemcpyHostToDevice);
        
        // copy contact volume gradient to vgrad_
        vgrad_.getDevice().Memcpy(vgrad_.begin(), &vGrad.front(), 
                vgrad_.size() * sizeof(value_type),
                vgrad_.getDevice().MemcpyHostToDevice);

        // get magnitude of projection direction
        // filter projection direction
        sht_filter_high(vgrad_, *xwrk, &sht_, params_.rep_exponent);
        CVJacobian(*xwrk, awrk);
        //CVJacobian(vgrad_, awrk);
        awrk.getDevice().xyInv(cvs.begin(), awrk.begin(), cvs.size(), awrk.begin());
        // projection direction
        CVJacobianTrans(awrk, *xwrk);
        
        /*
        // project to vgrad_
        // TODO: make sure vrad_ is normalized on every point
        // TODO: make twrk positive, then add to u1?
        GeometricDot(u1, vgrad_, *twrk);
        sca_abs(*twrk);
        INFO("project maxabs: "<<MaxAbs(*twrk));
        // get projected direction to substract
        xv(*twrk, vgrad_, *xwrk);
        axpy(static_cast<value_type>(-1.0), *xwrk, u1, u1);
        */
        axpy(static_cast<value_type>(-1.0), *xwrk, u1, u1);
        
        // get candidate position
        axpy(static_cast<value_type> (1.0), u1, x_old, *xwrk);

        // copy to pos_e as end configuration
        xwrk->getDevice().Memcpy(&pos_e.front(), xwrk->begin(),
                xwrk->size() * sizeof(value_type),
                xwrk->getDevice().MemcpyDeviceToHost);

        resolveCount++;
        INFO("Projecting to avoid collision iter#: "<<resolveCount);
        INFO(cvs);
        
        // test if still have contact
        if(params_.periodic_length > 0)
        {
            x_old.getDevice().Memcpy(&pos_s.front(), x_old.begin(),
                    x_old.size() * sizeof(value_type),
                    x_old.getDevice().MemcpyDeviceToHost);
            TransferVesicle(pos_s, pos_e);
        }
        CI_.getVolumeAndGradient(IV, num_cvs_, vGrad, vgrad_ind_, pos_s, pos_e, params_.min_sep_dist, params_.periodic_length);
    }

    recycle(xwrk);
    recycle(twrk);
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
sca_abs(Sca_t &xin) const
{
    value_type* xoi = xin.begin();
    
    size_t length = xin.size();

#pragma omp parallel for
    for (size_t idx = 0; idx < length; idx++)
    {
        xoi[idx] = (xoi[idx] > 0) ? xoi[idx] : -xoi[idx];
    }

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::
TransferVesicle(std::vector<value_type> &pos_s, std::vector<value_type> &pos_e) const
{
    // Set point_coord, point_value, poly_connect
    size_t p1 = params_.sh_order;
    size_t N_ves = pos_e.size()/(2*p1*(p1+1)*COORD_DIM); // Number of vesicles
    
    for(size_t k=0;k<N_ves;k++){ // Set point_coord
        value_type C[COORD_DIM]={0,0,0};
        for(long l=0;l<COORD_DIM;l++) C[l]=0;
        
        for(size_t i=0;i<p1+1;i++){
            for(size_t j=0;j<2*p1;j++){
                for(size_t l=0;l<COORD_DIM;l++){
                    C[l]+=pos_e[j+2*p1*(i+(p1+1)*(l+k*COORD_DIM))];
                }
            }
        }
      
        for(long l=0;l<COORD_DIM;l++) C[l]/=2*p1*(p1+1);
        for(long l=0;l<COORD_DIM;l++) C[l]=(floor(C[l]/params_.periodic_length))*params_.periodic_length;
      
        for(size_t i=0;i<p1+1;i++){
            for(size_t j=0;j<2*p1;j++){
                for(size_t l=0;l<COORD_DIM;l++){
                    pos_s[j+2*p1*(i+(p1+1)*(l+k*COORD_DIM))] -= C[l];
                    pos_e[j+2*p1*(i+(p1+1)*(l+k*COORD_DIM))] -= C[l];
                }
            }
        }
    }
    
    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
InterfacialVelocity<SurfContainer, Interaction>::value_type InterfacialVelocity<SurfContainer, Interaction>::
StokesError(const Vec_t &x) const
{
    PROFILESTART();
    stokes_.SetDensitySL(NULL);
    stokes_.SetDensityDL(NULL);
    stokes_.SetSrcCoord(x);
    value_type stokes_error=stokes_.MonitorError(params_.time_tol*0.1);
    PROFILEEND("",0);

    return stokes_error;
}

template <class Vec_t, class SHT>
static std::vector<typename Vec_t::value_type> inner_prod(const Vec_t& v1, const Vec_t& v2, SHT* sh_trans, int rep_exp){
  typedef typename Vec_t::value_type value_type;

  static Vec_t w, v1_, v2_;
  { // Set v1_
    v1_.replicate(v1);
    w  .replicate(v1);
    sh_trans->forward(v1, w, v1_);
  }
  { // Set v2_
    v2_.replicate(v2);
    w  .replicate(v2);
    sh_trans->forward(v2, w, v2_);
  }
  size_t p=v1.getShOrder();
  int ns_x = v1.getNumSubFuncs();

  assert(p<256);
  assert(rep_exp<128);
  static std::vector<value_type> A_[256*128];
  std::vector<value_type>& A=A_[rep_exp*256+p];
  if(!A.size()){
    A.resize(p+1);
    long filter_freq_=(rep_exp?p/2:p/3);
    for(int ii=0; ii<= p; ++ii){
      value_type a = 1.0 - (rep_exp?std::pow(ii*1.0/filter_freq_,rep_exp):0);
      a *= (ii > filter_freq_ ? 0.0 : 1.0 );
      A[ii] = 1.0 - a;
    }
  }

  std::vector<value_type> E(ns_x/COORD_DIM,0);
  for(int ii=0; ii<= p; ++ii){
    value_type* inPtr_v1 = v1_.begin() + ii;
    value_type* inPtr_v2 = v2_.begin() + ii;
    int len = 2*ii + 1 - (ii/p);
    for(int jj=0; jj< len; ++jj){
      int dist = (p + 1 - (jj + 1)/2);
      for(int ss=0; ss<ns_x; ++ss){
        E[ss/COORD_DIM] += A[ii]*(*inPtr_v1)*(*inPtr_v2);
        inPtr_v1 += dist;
        inPtr_v2 += dist;
      }
      inPtr_v1--;
      inPtr_v2--;
      inPtr_v1 += jj%2;
      inPtr_v2 += jj%2;
    }
  }

  return E;
}

template <class Vec_t, class SHT>
static void sht_filter(const Vec_t& v1, Vec_t& v2, SHT* sh_trans, int rep_exp){
  typedef typename Vec_t::value_type value_type;

  static Vec_t w, v1_;
  { // Set v1_
    v1_.replicate(v1);
    w  .replicate(v1);
    sh_trans->forward(v1, w, v1_);
  }
  size_t p=v1.getShOrder();
  int ns_x = v1.getNumSubFuncs();

  assert(p<256);
  assert(rep_exp<128);
  static std::vector<value_type> A_[256*128];
  std::vector<value_type>& A=A_[rep_exp*256+p];
  if(!A.size()){
    A.resize(p+1);
    long filter_freq_=(rep_exp?p/2:p/3);
    for(int ii=0; ii<= p; ++ii){
      value_type a = 1.0 - (rep_exp?std::pow(ii*1.0/filter_freq_,rep_exp):0);
      a *= (ii > filter_freq_ ? 0.0 : 1.0 );
      A[ii] = 1.0 - a;
    }
  }

  value_type E=0;
  for(int ii=0; ii<= p; ++ii){
    value_type* inPtr_v1 = v1_.begin() + ii;
    int len = 2*ii + 1 - (ii/p);
    for(int jj=0; jj< len; ++jj){
      int dist = (p + 1 - (jj + 1)/2);
      for(int ss=0; ss<ns_x; ++ss){
        inPtr_v1[0] *= -A[ii];
        inPtr_v1 += dist;
      }
      inPtr_v1--;
      inPtr_v1 += jj%2;
    }
  }

  { // Set v2
    v2 .replicate(v1);
    sh_trans->backward(v1_, w, v2);
  }
}

template <class Vec_t, class SHT>
static void sht_filter_high(const Vec_t& v1, Vec_t& v2, SHT* sh_trans, int rep_exp){
  typedef typename Vec_t::value_type value_type;

  static Vec_t wH, v1H_;
  { // Set v1_
    v1H_.replicate(v1);
    wH  .replicate(v1);
    sh_trans->forward(v1, wH, v1H_);
  }
  size_t p=v1.getShOrder();
  int ns_x = v1.getNumSubFuncs();

  assert(p<256);
  assert(rep_exp<128);
  static std::vector<value_type> AH_[256*128];
  std::vector<value_type>& AH=AH_[rep_exp*256+p];
  if(!AH.size()){
    AH.resize(p+1);
    long filter_freq_=(rep_exp?p/2:p/3);
    for(int ii=0; ii<= p; ++ii){
      value_type a = 1.0 - (rep_exp?std::pow(ii*1.0/filter_freq_,rep_exp):0);
      a *= (ii > filter_freq_ ? 0.0 : 1.0 );
      AH[ii] = a;
    }
  }

  value_type E=0;
  for(int ii=0; ii<= p; ++ii){
    value_type* inPtr_v1 = v1H_.begin() + ii;
    int len = 2*ii + 1 - (ii/p);
    for(int jj=0; jj< len; ++jj){
      int dist = (p + 1 - (jj + 1)/2);
      for(int ss=0; ss<ns_x; ++ss){
        inPtr_v1[0] *= AH[ii];
        inPtr_v1 += dist;
      }
      inPtr_v1--;
      inPtr_v1 += jj%2;
    }
  }

  { // Set v2
    v2 .replicate(v1);
    sh_trans->backward(v1H_, wH, v2);
  }
}

template<typename SurfContainer, typename Interaction>
Error_t InterfacialVelocity<SurfContainer, Interaction>::reparam()
{
    PROFILESTART();

    value_type ts(params_.rep_ts);
    value_type rep_tol(params_.rep_tol);
    long rep_maxit(params_.rep_maxit);
    long rep_exp=params_.rep_exponent;
    if(params_.rep_type==BoxReparam) rep_exp=0;

    // Set default values
    if(ts<0) ts=1e-3;
    if(rep_tol<0) rep_tol=1e-6;
    if(rep_maxit<0) rep_maxit=4000;

    SurfContainer* Surf;
    SHtrans_t* sh_trans;
    if (params_.rep_upsample){
        INFO("Upsampling for reparametrization");
        S_.resample(params_.upsample_freq, &S_up_);
        Surf = S_up_;
        sh_trans = &sht_upsample_;
    } else {
        Surf = &S_;
        sh_trans = &sht_;
    }

    // Storage for resolving collision
    std::auto_ptr<Vec_t> x_old = checkoutVec();
    std::auto_ptr<Vec_t> u1_down = checkoutVec();
    x_old ->replicate(S_.getPosition());
    u1_down ->replicate(S_.getPosition());
    // store collision free state in x_old
    // TODO: in upsample mode
    axpy(static_cast<value_type>(1.0), S_.getPosition(), *x_old);
    // End of storage for resolving collision

    std::auto_ptr<Vec_t> u1 = checkoutVec();
    std::auto_ptr<Vec_t> u2 = checkoutVec();
    std::auto_ptr<Sca_t> wrk = checkoutSca();
    u1 ->replicate(Surf->getPosition());
    u2 ->replicate(Surf->getPosition());
    wrk->replicate(Surf->getPosition());
    long N_ves = u1->getNumSubs();

    value_type E0=0;
    { // Compute energy E0
        std::vector<value_type>  x2=inner_prod(Surf->getPosition(), Surf->getPosition(), sh_trans, rep_exp);
        for(long i=0;i<x2.size();i++) E0+=x2[i];
    }
        
    int ii(0);
    std::vector<value_type> E;
    while ( ii < rep_maxit )
    {
        //Surf->getSmoothedShapePositionReparam(*u1);
        //axpy(static_cast<value_type>(-1), Surf->getPosition(), *u1, *u1);
        sht_filter(Surf->getPosition(), *u1, sh_trans, rep_exp);
        Surf->mapToTangentSpace(*u1, false /* upsample */);
        { // Filter u1
            static Vec_t u_, w;
            u_.replicate(*u1);
            w .replicate(*u1);
            sh_trans->forward(*u1, w, u_);
            sh_trans->backward(u_, w, *u1);
        }
        Surf->mapToTangentSpace(*u1, false /* upsample */);
        { // Filter u1
            static Vec_t u_, w;
            u_.replicate(*u1);
            w .replicate(*u1);
            sh_trans->forward(*u1, w, u_);
            sh_trans->backward(u_, w, *u1);
        }

        { // normalize u1 for each vesicle
            long N = u1->getNumSubFuncs();
            long M=u1->size()/N;
            value_type* u=u1->begin();
            for(long i=0;i<N/COORD_DIM;i++){
                value_type max_v=0;
                for(long j=0;j<M;j++){
                    value_type x=u[j+M*(0+i*COORD_DIM)];
                    value_type y=u[j+M*(1+i*COORD_DIM)];
                    value_type z=u[j+M*(2+i*COORD_DIM)];
                    max_v=std::max(max_v, sqrt(x*x+y*y+z*z));
                }
                for(long j=0;j<M;j++){
                    u[j+M*(0+i*COORD_DIM)]/=max_v;
                    u[j+M*(1+i*COORD_DIM)]/=max_v;
                    u[j+M*(2+i*COORD_DIM)]/=max_v;
                }
            }
        }

        std::vector<value_type>  x_dot_x =inner_prod(Surf->getPosition(), Surf->getPosition(), sh_trans, rep_exp);
        std::vector<value_type>  x_dot_u1=inner_prod(Surf->getPosition(),                 *u1, sh_trans, rep_exp);
        std::vector<value_type> u1_dot_u1=inner_prod(                *u1,                 *u1, sh_trans, rep_exp);

        value_type dt_max(0);
        std::vector<value_type> dt(x_dot_u1.size(),0);
        for(long i=0; i<N_ves; i++){
            dt[i]=std::min(ts, -x_dot_u1[i]/u1_dot_u1[i]);
            if( dt[i]<rep_tol || (E.size() && (E[i]==0 /*|| E[i]-x_dot_x[i]<rep_tol*rep_tol*E[i]*/ )) ){
                x_dot_x[i]=0;
                dt[i]=0;
            }
            dt_max=std::max(dt_max,dt[i]);

            long N=u1->getStride()*VES3D_DIM;
            value_type* u1_=u1->getSubN_begin(i);
            for(long j=0;j<N;j++) u1_[j]*=dt[i];
        }
        if(dt_max==0) break;
        E=x_dot_x;

        // begin for collision
        // project u1 to collision free
        //INFO("Begin Project reparam direction to without contact.");
        //projectU1(*u1, Surf->getPosition());
        //INFO("End Project reparam direction to without contact.");
        // end for collision

        //Advecting tension (useless for implicit)
        if (params_.scheme != GloballyImplicit){
            if (params_.rep_upsample)
                WARN("Reparametrizaition is not advecting the tension in the upsample mode (fix!)");
            else {
                Surf->grad(tension_, *u2);
                GeometricDot(*u2, *u1, *wrk);
                axpy(1.0, *wrk, tension_, tension_);
            }
        }

        //updating position
        axpy(1.0, *u1, Surf->getPosition(), Surf->getPositionModifiable());

        COUTDEBUG("Iteration = "<<ii<<", dt = "<<dt_max);
        ++ii;
    }

    value_type E1=0;
    { // Compute energy E1
        std::vector<value_type>  x2=inner_prod(Surf->getPosition(), Surf->getPosition(), sh_trans, rep_exp);
        for(long i=0;i<x2.size();i++) E1+=x2[i];
    }
    INFO("Iterations = "<<ii<<", Energy = "<<E1<<", dE = "<<E1-E0);
    { // print log(coeff)
      std::auto_ptr<Vec_t> x = checkoutVec();
      { // Set x
        std::auto_ptr<Vec_t> w   = checkoutVec();
        x  ->replicate(Surf->getPosition());
        w  ->replicate(Surf->getPosition());
        sh_trans->forward(Surf->getPosition(), *w, *x);
        recycle(w);
      }
      {
          size_t p=x->getShOrder();
          int ns_x = x->getNumSubFuncs();
          std::vector<value_type> coeff_norm0(p+1,0);
          for(int ii=0; ii<= p; ++ii){
              value_type* inPtr_x = x->begin() + ii;

              int len = 2*ii + 1 - (ii/p);
              for(int jj=0; jj< len; ++jj){

                  int dist = (p + 1 - (jj + 1)/2);
                  for(int ss=0; ss<ns_x; ++ss){
                      coeff_norm0[ii] = std::max(coeff_norm0[ii], (*inPtr_x)*(*inPtr_x));
                      inPtr_x += dist;
                  }
                  inPtr_x--;
                  inPtr_x += jj%2;
              }
          }

          std::stringstream ss;
          ss<<"SPH-Coeff0: ";
          for(int ii=0; ii<= p; ++ii){
            ss<<-(int)(0.5-10*log(sqrt(coeff_norm0[ii]))/log(10.0))*0.1<<' ';
          }
          ss<<'\n';
          INFO(ss.str());
      }
      recycle(x);
    }

    if (params_.rep_upsample)
        Resample(Surf->getPosition(), sht_upsample_, sht_, *u1, *u2,
            S_.getPositionModifiable());

    // begin for collision
    // project u1 to collision free
    INFO("Begin Project reparam direction to without contact.");
    axpy(static_cast<value_type>(-1.0), *x_old, S_.getPosition(), *u1_down);
    projectU1(*u1_down, *x_old);
    axpy(static_cast<value_type>(1.0), *u1_down, *x_old, S_.getPositionModifiable());
    INFO("End Project reparam direction to without contact.");
    // end for collision

    recycle(u1);
    recycle(u2);
    recycle(x_old);
    recycle(u1_down);
    recycle(wrk);
    PROFILEEND("",0);

    return ErrorEvent::Success;
}

template<typename SurfContainer, typename Interaction>
std::auto_ptr<typename SurfContainer::Sca_t> InterfacialVelocity<SurfContainer, Interaction>::
checkoutSca() const
{
    std::auto_ptr<Sca_t> scp;

    if(scalar_work_q_.empty())
        scp = static_cast<std::auto_ptr<Sca_t> >(new Sca_t);
    else
    {
        scp = static_cast<std::auto_ptr<Sca_t> >(scalar_work_q_.front());
        scalar_work_q_.pop();
    }

    scp->replicate(S_.getPosition());
    ++checked_out_work_sca_;
    return(scp);
}
template<typename SurfContainer, typename Interaction>
void InterfacialVelocity<SurfContainer, Interaction>::
recycle(std::auto_ptr<Sca_t> scp) const
{
    scalar_work_q_.push(scp.release());
    --checked_out_work_sca_;
}

template<typename SurfContainer, typename Interaction>
std::auto_ptr<typename SurfContainer::Vec_t> InterfacialVelocity<SurfContainer, Interaction>::
checkoutVec() const
{
    std::auto_ptr<Vec_t> vcp;

    if(vector_work_q_.empty())
        vcp = static_cast<std::auto_ptr<Vec_t> >(new Vec_t);
    else
    {
        vcp = static_cast<std::auto_ptr<Vec_t> >(vector_work_q_.front());
        vector_work_q_.pop();
    }

    vcp->replicate(S_.getPosition());
    ++checked_out_work_vec_;

    return(vcp);
}

template<typename SurfContainer, typename Interaction>
void InterfacialVelocity<SurfContainer, Interaction>::
recycle(std::auto_ptr<Vec_t> vcp) const
{
    vector_work_q_.push(vcp.release());
    --checked_out_work_vec_;
}

template<typename SurfContainer, typename Interaction>
void InterfacialVelocity<SurfContainer, Interaction>::
purgeTheWorkSpace() const
{
    while ( !scalar_work_q_.empty() )
    {
         delete scalar_work_q_.front();
        scalar_work_q_.pop();
    }

    while ( !vector_work_q_.empty() )
    {
        delete vector_work_q_.front();
        vector_work_q_.pop();
    }
}
