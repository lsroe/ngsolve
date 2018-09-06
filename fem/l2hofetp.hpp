namespace ngfem
{

  template <class FEL, class SHAPES, ELEMENT_TYPE ET, 
            class BASE = ScalarFiniteElement<ET_trait<ET>::DIM> >

  class T_ScalarFiniteElementTP : public T_ScalarFiniteElement<SHAPES,ET,BASE>
  {
    
    auto & Cast() const { return static_cast<const FEL&> (*this); }


    virtual void Evaluate (const SIMD_IntegrationRule & ir,
                           BareSliceVector<> coefs,
                           BareVector<SIMD<double>> values) const override
    {
      // Vector<SIMD<double>> val1(ir.Size());
      // T_ScalarFiniteElement<SHAPES,ET,BASE>::Evaluate (ir, coefs, val1);      
      
      if (ir.IsTP())
        {
          static Timer tcnt("Evaluate - count");
          static Timer t("Evaluate - fast");
          static Timer tcopy("Evaluate - fast reorder");
          static Timer tx("Evaluate - fast x");
          static Timer ty("Evaluate - fast y");
          static Timer tz("Evaluate - fast z");
          static Timer txmult("Evaluate - fast x mult");
          static Timer tymult("Evaluate - fast y mult");
          static Timer tzmult("Evaluate - fast z mult");
          
          ThreadRegionTimer reg(t, TaskManager::GetThreadId());

          auto & irx = ir.GetIRX();
          auto & iry = ir.GetIRY();
          auto & irz = ir.GetIRZ();
          
          size_t nipx = irx.GetNIP();
          size_t nipy = iry.GetNIP();
          size_t nipz = irz.GetNIP();
          NgProfiler::AddThreadFlops (t, TaskManager::GetThreadId(), nipx*nipy*nipz*this->ndof);
          NgProfiler::AddThreadFlops (tcnt, TaskManager::GetThreadId(), 1);
          
          size_t ndof1d = static_cast<const FEL&> (*this).GetNDof1d ();
          size_t ndof2d = static_cast<const FEL&> (*this).GetNDof2d ();

          size_t order = this->order;

          STACK_ARRAY(double, mem_cube, (order+1)*(order+1)*(order+1));
          FlatMatrix<> cube_coefs(sqr(order+1), order+1, &mem_cube[0]);

          {
          ThreadRegionTimer regcopy(tcopy, TaskManager::GetThreadId());
          NgProfiler::AddThreadFlops (tcopy, TaskManager::GetThreadId(), this->ndof);
          for (size_t iz = 0, ii = 0, icube=0; iz <= order; iz++, icube+=sqr(order+1))
            for (size_t iy = 0, icube2 = icube; iy <= order-iz; iy++, icube2+=order+1)
              for (size_t ix = 0; ix <= order-iz-iy; ix++, ii++)
                cube_coefs(icube2+ix) = coefs(ii);
          }
          
          STACK_ARRAY(double, mem_quad, (order+1)*(order+1)*nipx);
          FlatMatrix<> quad_coefs(sqr(order+1), nipx, &mem_quad[0]);
           

          {
            ThreadRegionTimer reg(tx, TaskManager::GetThreadId());          
            int nshapex = static_cast<const FEL&> (*this).NShapeX();
            
            STACK_ARRAY(SIMD<double>, mem_facx, nshapex*irx.Size());          
            FlatMatrix<SIMD<double>> simd_facx(nshapex, irx.Size(), &mem_facx[0]);
            Cast().CalcXFactor(irx, simd_facx);
            SliceMatrix<double> facx(nshapex, nipx, irx.Size()*SIMD<double>::Size(), &simd_facx(0,0)[0]);
            
            ThreadRegionTimer regmult(txmult, TaskManager::GetThreadId());                                  
            for (size_t shapenr = 0; shapenr <= order; shapenr++)
              {
                /*
                MultMatMat (cube_coefs.RowSlice(shapenr, order).Cols(0, order+1-shapenr).AddSize(shapenr+1, order+1-shapenr),
                            facx.Rows(shapenr*(order+1), shapenr*(order+1)+order+1-shapenr),
                            quad_coefs.RowSlice(shapenr, order).AddSize(shapenr+1, nipx));
                */
                quad_coefs.RowSlice(shapenr, order).AddSize(shapenr+1, nipx) =
                  cube_coefs.RowSlice(shapenr, order).Cols(0, order+1-shapenr).AddSize(shapenr+1, order+1-shapenr)
                  * facx.Rows(shapenr*(order+1), shapenr*(order+1)+order+1-shapenr);
              
                NgProfiler::AddThreadFlops (txmult, TaskManager::GetThreadId(), nipx*(order+1-shapenr)*(shapenr+1));
              }
          }

          STACK_ARRAY(double, mem_trans1, ndof1d*nipx*nipy);
          FlatMatrix<> trans1(ndof1d, nipx*nipy, &mem_trans1[0]);

          {
            ThreadRegionTimer reg(ty, TaskManager::GetThreadId());
            STACK_ARRAY(SIMD<double>, mem_facy, ndof2d*iry.Size());
            FlatMatrix<SIMD<double>> simd_facy(ndof2d, iry.Size(), &mem_facy[0]);
            Cast().CalcYFactor(iry, simd_facy);          
            SliceMatrix<double> facy(ndof2d, nipy, iry.Size()*SIMD<double>::Size(), &simd_facy(0,0)[0]);
            
            ThreadRegionTimer regmult(tymult, TaskManager::GetThreadId());
            NgProfiler::AddThreadFlops (tymult, TaskManager::GetThreadId(), nipx*nipy*ndof2d);            

            static_cast<const FEL&> (*this).
              Map1t2([facy, trans1, quad_coefs, order, nipx, nipy] (size_t iy, IntRange r)
                     {
                       FlatMatrix<double> trans1xy(nipy, nipx, &trans1(iy,0));
                       /*
                       MultAtB (facy.Rows(r),
                       quad_coefs.Rows(iy*(order+1), (iy+1)*(order+1)-iy),
                                trans1xy);
                       */
                       trans1xy = Trans(facy.Rows(r)) * quad_coefs.Rows(iy*(order+1), (iy+1)*(order+1)-iy);
                     });

            /*
            STACK_ARRAY(SIMD<double>, mem_facy, (order+1)*iry.Size());
            FlatMatrix<SIMD<double>> simd_facy(order+1, iry.Size(), &mem_facy[0]);
            SliceMatrix<double> facy(order+1, nipy, iry.Size()*SIMD<double>::Size(), &simd_facy(0,0)[0]);
            
            ThreadRegionTimer regmult(tymult, TaskManager::GetThreadId());
            NgProfiler::AddThreadFlops (tymult, TaskManager::GetThreadId(), nipx*nipy*ndof2d);            
            static_cast<const FEL&> (*this).
              Map1t2([this,&iry,facy, simd_facy, trans1, quad_coefs, order, nipx, nipy] (size_t iy, IntRange r)
                     {
                       static_cast<const FEL&> (*this).CalcYFactor(iy, iry, simd_facy);
                       FlatMatrix<double> trans1xy(nipy, nipx, &trans1(iy,0));
                       MultAtB (facy.Rows(0, r.Size()), 
                                quad_coefs.Rows(iy*(order+1), (iy+1)*(order+1)-iy),
                                trans1xy);
                     });
            */
          }
          
          
          {
            ThreadRegionTimer reg(tz, TaskManager::GetThreadId());
            NgProfiler::AddThreadFlops (tzmult, TaskManager::GetThreadId(), nipx*nipy*nipz*ndof1d);

            FlatMatrix<double> hvalues(nipz, nipx*nipy,  &values(0)[0]);
            STACK_ARRAY(SIMD<double>, mem_facz, ndof1d*irz.Size());
            FlatMatrix<SIMD<double>> simd_facz(ndof1d, irz.Size(), &mem_facz[0]);
            SliceMatrix<double> facz(ndof1d, nipz, irz.Size()*SIMD<double>::Size(), &simd_facz(0,0)[0]);
            for (auto iz : Range(irz))
              Cast().CalcZFactorIP(irz[iz](0), simd_facz.Col(iz));

            ThreadRegionTimer regmult(tzmult, TaskManager::GetThreadId());            
            // MultAtB (facz, trans1, hvalues);
            hvalues = Trans(facz) * trans1;
          }
 
          // cout << "diff-norm = " << L2Norm(values.AddSize(ir.Size())-val1) << endl;
          return;
        }
      T_ScalarFiniteElement<SHAPES,ET,BASE>::Evaluate (ir, coefs, values);
    }

    
    virtual void AddTrans (const SIMD_IntegrationRule & ir,
                           BareVector<SIMD<double>> values,
                           BareSliceVector<> coefs) const override
    {
      
      if (ir.IsTP())
        {
          static Timer tcnt("Add Trans - fast cnt");
          static Timer tadd("Add Trans - add");
          static Timer t("Add Trans - fast");
          static Timer tx("Add Trans - fast x");
          static Timer ty("Add Trans - fast y");
          static Timer tz("Add Trans - fast z");
          static Timer txmult("Add Trans - fast x mult");
          static Timer tymult("Add Trans - fast y mult");
          static Timer tzmult("Add Trans - fast z mult");

          static Timer ty_repeat("Add Trans - fast y repeat");
          
          ThreadRegionTimer reg(t, TaskManager::GetThreadId());
          
          auto & irx = ir.GetIRX();
          auto & iry = ir.GetIRY();
          auto & irz = ir.GetIRZ();
          
          size_t nipx = irx.GetNIP();
          size_t nipy = iry.GetNIP();
          size_t nipz = irz.GetNIP();

          size_t ndof = this->ndof;
          size_t ndof1d = static_cast<const FEL&> (*this).GetNDof1d ();
          size_t ndof2d = static_cast<const FEL&> (*this).GetNDof2d ();

          STACK_ARRAY(double, mem_trans1, ndof1d*nipx*nipy);
          FlatMatrix<> trans1(ndof1d, nipx*nipy, &mem_trans1[0]);
          NgProfiler::AddThreadFlops (t, TaskManager::GetThreadId(), nipx*nipy*nipz*ndof);
          NgProfiler::AddThreadFlops (tcnt, TaskManager::GetThreadId(), 1);
          
          {
            ThreadRegionTimer reg(tz, TaskManager::GetThreadId());
            NgProfiler::AddThreadFlops (tzmult, TaskManager::GetThreadId(), nipx*nipy*nipz*ndof1d);

            FlatMatrix<double> hvalues(nipz, nipx*nipy,  &values(0)[0]);
            STACK_ARRAY(SIMD<double>, mem_facz, ndof1d*irz.Size());
            FlatMatrix<SIMD<double>> simd_facz(ndof1d, irz.Size(), &mem_facz[0]);
            SliceMatrix<double> facz(ndof1d, nipz, irz.Size()*SIMD<double>::Size(), &simd_facz(0,0)[0]);
            for (auto iz : Range(irz))
              Cast().CalcZFactorIP(irz[iz](0), simd_facz.Col(iz));
            // static_cast<const FEL&> (*this).CalcZFactor(irz, simd_facz);

            ThreadRegionTimer regmult(tzmult, TaskManager::GetThreadId());            
            // MultMatMat (facz, hvalues, trans1);
            trans1 = facz * hvalues;
          }

          STACK_ARRAY(double, mem_trans2, ndof2d*nipx);
          FlatMatrix<> trans2(ndof2d, nipx, &mem_trans2[0]);
          
          {
            ThreadRegionTimer reg(ty, TaskManager::GetThreadId());          

            STACK_ARRAY(SIMD<double>, mem_facy, ndof2d*iry.Size());
            FlatMatrix<SIMD<double>> simd_facy(ndof2d, iry.Size(), &mem_facy[0]);
            Cast().CalcYFactor(iry, simd_facy);          
            SliceMatrix<double> facy(ndof2d, nipy, iry.Size()*SIMD<double>::Size(), &simd_facy(0,0)[0]);          

            ThreadRegionTimer regmult(tymult, TaskManager::GetThreadId());
            NgProfiler::AddThreadFlops (tymult, TaskManager::GetThreadId(), nipx*nipy*ndof2d);

            /*
            for (size_t i = 0; i < ndof1d; i++)
              {
                IntRange r = static_cast<const FEL&> (*this).Map1t2(i);
                FlatMatrix<double> trans1xy(nipy, nipx, &trans1(i,0));
                MultMatMat (facy.Rows(r), trans1xy, trans2.Rows(r));
              }
            */

            Cast().Map1t2([facy, trans1, trans2, nipx, nipy] (size_t iy, IntRange r)
                          {
                            FlatMatrix<double> trans1xy(nipy, nipx, &trans1(iy,0));
                            // MultMatMat (facy.Rows(r), trans1xy, trans2.Rows(r));
                            trans2.Rows(r) = facy.Rows(r) * trans1xy;
                          });
          }

          
          
          {
            ThreadRegionTimer reg(tx, TaskManager::GetThreadId());          
            int nshapex = static_cast<const FEL&> (*this).NShapeX();
            
            STACK_ARRAY(SIMD<double>, mem_facx, nshapex*irx.Size());          
            FlatMatrix<SIMD<double>> simd_facx(nshapex, irx.Size(), &mem_facx[0]);
            Cast().CalcXFactor(irx, simd_facx);
            SliceMatrix<double> facx(nshapex, nipx, irx.Size()*SIMD<double>::Size(), &simd_facx(0,0)[0]);

            STACK_ARRAY(double, mem_trans3, this->ndof);
            FlatVector<> trans3(this->ndof, &mem_trans3[0]);

            ThreadRegionTimer regmult(txmult, TaskManager::GetThreadId());
            NgProfiler::AddThreadFlops (txmult, TaskManager::GetThreadId(), nipx*this->ndof);            
            Cast().
              Map2t3([facx, trans2, trans3] (INT<4, size_t> i4) // base 3, base 2, base x, nr
                     {
                       /*
                       MultMatVec (facx.Rows(i4[2], i4[2]+i4[3]),
                                   trans2.Row(i4[1]),
                                   trans3.Range(i4[0], i4[0]+i4[3]));
                       */
                       trans3.Range(i4[0], i4[0]+i4[3]) = facx.Rows(i4[2], i4[2]+i4[3]) * trans2.Row(i4[1]);
                     });

            {
              ThreadRegionTimer regadd(tadd, TaskManager::GetThreadId());                                  
              coefs.AddSize(this->ndof) += trans3;
            }
          }

#ifdef JUSTFORTST
          {
            // for testing: 4 tets simultaneously
            STACK_ARRAY (SIMD<double>, mem_vcoefs, this->ndof);
            FlatVector<SIMD<double>> vcoefs(this->ndof, &mem_vcoefs[0]);
            STACK_ARRAY (SIMD<double>, mem_vvalues, nipx*nipy*nipz);
            FlatVector<SIMD<double>> vvalues(nipx*nipy*nipz, &mem_vvalues[0]);

            FlatVector<> svalues(nipx*nipy*nipz, &values(0)[0]);
            vvalues = svalues;

            STACK_ARRAY(SIMD<double>, mem_trans1, ndof1d*nipx*nipy);
            FlatMatrix<double> trans1(ndof1d, 4*nipx*nipy, &mem_trans1[0][0]);

            {
              static Timer tzv("Add Trans - fast z vec");
              static Timer tzmultv("Add Trans - fast z mult vec");
              
              ThreadRegionTimer reg(tzv, TaskManager::GetThreadId());
              NgProfiler::AddThreadFlops (tzmultv, TaskManager::GetThreadId(), 4*nipx*nipy*nipz*ndof1d);
              
              FlatMatrix<double> hvalues(nipz, 4*nipx*nipy,  &vvalues(0)[0]);
              STACK_ARRAY(SIMD<double>, mem_facz, ndof1d*irz.Size());
              FlatMatrix<SIMD<double>> simd_facz(ndof1d, irz.Size(), &mem_facz[0]);
              SliceMatrix<double> facz(ndof1d, nipz, irz.Size()*SIMD<double>::Size(), &simd_facz(0,0)[0]);
              // static_cast<const FEL&> (*this).CalcZFactor(irz, simd_facz);
              for (size_t iz : Range(irz))
                Cast().CalcZFactorIP(irz[i](0), simd_facz.Col(iz));
              
              ThreadRegionTimer regmult(tzmultv, TaskManager::GetThreadId());            
              MultMatMat (facz, hvalues, trans1);
            }

            
            STACK_ARRAY(SIMD<double>, mem_trans2, ndof2d*nipx);
            FlatMatrix<> trans2(ndof2d, 4*nipx, &mem_trans2[0][0]);
            
            {
              static Timer tyv("Add Trans - fast y vec");
              static Timer tymultv("Add Trans - fast y mult vec");
              
              ThreadRegionTimer reg(tyv, TaskManager::GetThreadId());          
              
              STACK_ARRAY(SIMD<double>, mem_facy, ndof2d*iry.Size());
              FlatMatrix<SIMD<double>> simd_facy(ndof2d, iry.Size(), &mem_facy[0]);
              Cast().CalcYFactor(iry, simd_facy);          
              SliceMatrix<double> facy(ndof2d, nipy, iry.Size()*SIMD<double>::Size(), &simd_facy(0,0)[0]);          

              ThreadRegionTimer regmult(tymultv, TaskManager::GetThreadId());
              NgProfiler::AddThreadFlops (tymultv, TaskManager::GetThreadId(), 4*nipx*nipy*ndof2d);

              for (size_t i = 0; i < ndof1d; i++)
                {
                  IntRange r = static_cast<const FEL&> (*this).Map1t2(i);
                  FlatMatrix<double> trans1xy(nipy, 4*nipx, &trans1(i,0));
                  MultMatMat (facy.Rows(r), trans1xy, trans2.Rows(r));
                }
            }
            

            
          }
#endif
          return;
        }
      T_ScalarFiniteElement<SHAPES,ET,BASE>::AddTrans (ir, values, coefs);
    }
    
    
    virtual void EvaluateGrad (const SIMD_BaseMappedIntegrationRule & mir,
                               BareSliceVector<> bcoefs,
                               BareSliceMatrix<SIMD<double>> values) const override
    {
      const SIMD_IntegrationRule & ir = mir.IR();

      // Matrix<SIMD<double>> val1(3,ir.Size());
      // T_ScalarFiniteElement<SHAPES,ET,BASE>::EvaluateGrad (mir, bcoefs, val1);      
      
      if (ir.IsTP())
        {
          static Timer t("Eval Grad - fast");
          static Timer tsetup("Eval Grad - setup");
          static Timer tcalc("Eval Grad - calc");
          static Timer tcalcx("Eval Grad - calcx");
          static Timer tcalcy("Eval Grad - calcy");
          static Timer tcalcz("Eval Grad - calcz");

          static Timer ttransx("Eval Grad - transx");
          static Timer ttransy("Eval Grad - transy");
          static Timer ttransz("Eval Grad - transz");
          static Timer ttransjac("Eval Grad - trans jacobi");

          ThreadRegionTimer reg(t, TaskManager::GetThreadId());

          STACK_ARRAY (double, mem_coefs, this->ndof);
          FlatVector<> coefs(this->ndof, &mem_coefs[0]);
          coefs = bcoefs;
          
          auto & irx = ir.GetIRX();
          auto & iry = ir.GetIRY();
          auto & irz = ir.GetIRZ();
	  
          size_t nipx = irx.GetNIP();
          size_t nipy = iry.GetNIP();
          size_t nipz = irz.GetNIP();

          size_t nipxy = nipx*nipy;
          size_t nip = nipx * nipy * nipz;
          size_t ndof = this->ndof;
          size_t ndof1d = static_cast<const FEL&> (*this).GetNDof1d ();
          size_t ndof2d = static_cast<const FEL&> (*this).GetNDof2d ();

          int nshapex = Cast().NShapeX();          
          STACK_ARRAY(SIMD<double>, mem_facx, 2*nshapex*irx.Size());
          FlatMatrix<SIMD<double> > facx(ndof, irx.Size(), &mem_facx[0]);
          FlatMatrix<SIMD<double> > facdx(ndof, irx.Size(), &mem_facx[nshapex*irx.Size()]);

          STACK_ARRAY(SIMD<double>, mem_facy, 2*ndof2d*iry.Size());          
          FlatMatrix<SIMD<double> > facy(ndof2d, iry.Size(), &mem_facy[0]);
          FlatMatrix<SIMD<double> > facdy(ndof2d, iry.Size(), &mem_facy[ndof2d*iry.Size()]);

          STACK_ARRAY(SIMD<double>, mem_facz, 2*ndof1d*irz.Size());          
          FlatMatrix<SIMD<double> > facz(ndof1d, irz.Size(), &mem_facz[0]);
          FlatMatrix<SIMD<double> > facdz(ndof1d, irz.Size(), &mem_facz[ndof1d*irz.Size()]);

          STACK_ARRAY(SIMD<double>, memx, irx.Size());
          STACK_ARRAY(SIMD<double>, memy, iry.Size());
          STACK_ARRAY(SIMD<double>, memz, irz.Size());
          FlatVector<SIMD<double>> vecx(irx.Size(), &memx[0]);
          FlatVector<SIMD<double>> vecy(iry.Size(), &memy[0]);
          FlatVector<SIMD<double>> vecz(irz.Size(), &memz[0]);

          
          for (size_t i1 = 0; i1 < irz.Size(); i1++)
            {
              vecz(i1) = irz[i1](0);                            
              AutoDiff<1,SIMD<double>> z (irz[i1](0), 0);
              Cast().CalcZFactorIP
                (z, SBLambda ([&](int iz, auto shape)
                              {
                                facz(iz, i1) = shape.Value();
                                facdz(iz, i1) = shape.DValue(0);
                              }));
            }
    
          for (size_t i1 = 0; i1 < iry.Size(); i1++)
            {
              vecy(i1) = iry[i1](0);              
              AutoDiff<1,SIMD<double>> y (iry[i1](0), 0);
              Cast().CalcYFactorIP
                (y, SBLambda ([&](int iy, auto shape)
                              {
                                facy(iy, i1) = shape.Value();
                                facdy(iy, i1) = shape.DValue(0);
                              }));
            }
          
          for (size_t i1 = 0; i1 < irx.Size(); i1++)
            {
              vecx(i1) = irx[i1](0);
              AutoDiff<1,SIMD<double>> x (irx[i1](0), 0);
              Cast().CalcXFactorIP
                (x, SBLambda ([&](int ix, auto shape)
                              {
                                facx(ix, i1) = shape.Value();
                                facdx(ix, i1) = shape.DValue(0);
                              }));
            }

          SliceMatrix<double> facz_ref(ndof1d, irz.GetNIP(), SIMD<double>::Size()*irz.Size(), &facz(0)[0]);
          SliceMatrix<double> facdz_ref(ndof1d, irz.GetNIP(), SIMD<double>::Size()*irz.Size(), &facdz(0)[0]);

          SliceMatrix<double> facy_ref(ndof2d, iry.GetNIP(), SIMD<double>::Size()*iry.Size(), &facy(0)[0]);
          SliceMatrix<double> facdy_ref(ndof2d, iry.GetNIP(), SIMD<double>::Size()*iry.Size(), &facdy(0)[0]);
          
          SliceMatrix<double> facx_ref(ndof, irx.GetNIP(), SIMD<double>::Size()*irx.Size(), &facx(0)[0]);
          SliceMatrix<double> facdx_ref(ndof, irx.GetNIP(), SIMD<double>::Size()*irx.Size(), &facdx(0)[0]);
          
          STACK_ARRAY(double, mem_gridx, 2*ndof2d*nipx);
          FlatMatrix<double> gridx(ndof2d, nipx, &mem_gridx[0]);
          FlatMatrix<double> gridx_dx(ndof2d, nipx, &mem_gridx[ndof2d*nipx]);

          STACK_ARRAY(double, mem_gridxy, 3*ndof1d*nipxy);          
          FlatMatrix<double> gridxy(ndof1d, nipx*nipy, &mem_gridxy[0]);
          FlatMatrix<double> gridxy_dx(ndof1d, nipx*nipy, &mem_gridxy[ndof1d*nipxy]);
          FlatMatrix<double> gridxy_dy(ndof1d, nipx*nipy, &mem_gridxy[2*ndof1d*nipxy]);
          
          // Vector<SIMD<double>> grid_dx(ir.Size());
          // Vector<SIMD<double>> grid_dy(ir.Size());
          // Vector<SIMD<double>> grid_dz(ir.Size());
          FlatMatrix<> mgrid_dx(nipz, nipx*nipy, &values(0,0)[0]);
          FlatMatrix<> mgrid_dy(nipz, nipx*nipy, &values(1,0)[0]);
          FlatMatrix<> mgrid_dz(nipz, nipx*nipy, &values(2,0)[0]);
          values.Col(ir.Size()-1).AddSize(3) = SIMD<double>(0);
          // grid_dx(ir.Size()-1) = SIMD<double>(0);
          // grid_dy(ir.Size()-1) = SIMD<double>(0);
          // grid_dz(ir.Size()-1) = SIMD<double>(0);
          
          FlatVector<double> vecx_ref(irx.GetNIP(), &vecx(0)[0]);
          FlatVector<double> vecy_ref(iry.GetNIP(), &vecy(0)[0]);
          FlatVector<double> vecz_ref(irz.GetNIP(), &vecz(0)[0]);

          {
          ThreadRegionTimer regcalc(tcalc, TaskManager::GetThreadId());

          {
          ThreadRegionTimer regcalc(tcalcx, TaskManager::GetThreadId());
          NgProfiler::AddThreadFlops (tcalcx, TaskManager::GetThreadId(), 2*nipx*ndof);
          Cast().
            Map2t3([facx_ref, facdx_ref, coefs, &gridx, &gridx_dx] (INT<4, size_t> i4)
                   // base 3, base 2, base x, nr
                   {
                     size_t base3 = i4[0];
                     size_t base2 = i4[1];
                     size_t basex = i4[2];
                     size_t cnt = i4[3];
                     /*
                     gridx.Row(base2) = Trans(facx_ref.Rows(basex,basex+cnt)) * coefs.Range(base3, base3+cnt);
                     gridx_dx.Row(base2) = Trans(facdx_ref.Rows(basex,basex+cnt)) * coefs.Range(base3, base3+cnt);
                     */
                     MultMatTransVec (facx_ref .Rows(basex,basex+cnt), coefs.Range(base3, base3+cnt), gridx   .Row(base2));
                     MultMatTransVec (facdx_ref.Rows(basex,basex+cnt), coefs.Range(base3, base3+cnt), gridx_dx.Row(base2));
                   });          
          }

          {
          ThreadRegionTimer regcalc(ttransx, TaskManager::GetThreadId());          
          for (size_t ix = 0; ix < nipx; ix++)
            gridx.Col(ix) *= 1.0 / (1-vecx_ref(ix));
          }
          
          {
          ThreadRegionTimer regcalc(tcalcy, TaskManager::GetThreadId());
          NgProfiler::AddThreadFlops (tcalcy, TaskManager::GetThreadId(), 3*nipxy*ndof2d);          
          Cast().
            Map1t2([&] (size_t i1d, IntRange r) 
                   {
                     FlatMatrix<> mgridxy(nipy, nipx, &gridxy(i1d,0));
                     FlatMatrix<> mgridxy_dx(nipy, nipx, &gridxy_dx(i1d,0));
                     FlatMatrix<> mgridxy_dy(nipy, nipx, &gridxy_dy(i1d,0));

                     /*
                     mgridxy    = Trans(facy_ref.Rows(r)) * gridx.Rows(r);
                     mgridxy_dx = Trans(facy_ref.Rows(r)) * gridx_dx.Rows(r);
                     mgridxy_dy = Trans(facdy_ref.Rows(r)) * gridx.Rows(r);
                     */
                     MultAtB (facy_ref .Rows(r), gridx   .Rows(r), mgridxy);
                     MultAtB (facy_ref .Rows(r), gridx_dx.Rows(r), mgridxy_dx);
                     MultAtB (facdy_ref.Rows(r), gridx   .Rows(r), mgridxy_dy);
                   });          
          }

          {
          ThreadRegionTimer regcalc(ttransy, TaskManager::GetThreadId());
          /*
          for (size_t iy = 0, ixy = 0; iy < iry.GetNIP(); iy++)
            for (size_t ix = 0; ix < irx.GetNIP(); ix++, ixy++)
              {
                gridxy_dx.Col(ixy) += vecy_ref(iy) * gridxy_dy.Col(ixy);
                gridxy.Col(ixy) *= 1.0 / (1-vecy_ref(iy));
              }
          */
          for (size_t iy = 0, ixy = 0; iy < iry.GetNIP(); iy++, ixy+=nipx)
            {
              double y = vecy_ref(iy);
              IntRange cols(ixy, ixy+nipx);
              gridxy_dx.Cols(cols) += y * gridxy_dy.Cols(cols);
              gridxy.Cols(cols) *= 1/(1-y);
            }

          
          }
          /*
          mgrid_dx = Trans(facz_ref) * gridxy_dx;    
          mgrid_dy = Trans(facz_ref) * gridxy_dy;    
          mgrid_dz = Trans(facdz_ref) * gridxy;    
          */
          {
            ThreadRegionTimer regcalc(tcalcz, TaskManager::GetThreadId());
            NgProfiler::AddThreadFlops (tcalcz, TaskManager::GetThreadId(), 3*nipxy*nipz*ndof1d);            
            MultAtB (facz_ref , gridxy_dx, mgrid_dx);
            MultAtB (facz_ref , gridxy_dy, mgrid_dy);
            MultAtB (facdz_ref, gridxy   , mgrid_dz);
          }

          {
            ThreadRegionTimer regcalc(ttransz, TaskManager::GetThreadId());                    
            for (int iz = 0; iz < nipz; iz++)
              {
                double z = vecz_ref(iz);
                mgrid_dx.Row(iz) += z * mgrid_dz.Row(iz);
                mgrid_dy.Row(iz) += z * mgrid_dz.Row(iz);
              }
          }
          // values.Row(0).AddSize(ir.Size()) = grid_dx;
          // values.Row(1).AddSize(ir.Size()) = grid_dy;
          // values.Row(2).AddSize(ir.Size()) = grid_dz;
          ThreadRegionTimer regjac(ttransjac, TaskManager::GetThreadId());                              
          mir.TransformGradient (values);
          }

          // cout << "diff = " << L2Norm2( (values.AddSize(3,ir.Size()) - val1)) << endl;
          return;
        }
      T_ScalarFiniteElement<SHAPES,ET,BASE>::EvaluateGrad (ir, bcoefs, values);      
    }
    
  };

  


  
  template <ELEMENT_TYPE ET>
  class L2HighOrderFETP : public T_ScalarFiniteElementTP<L2HighOrderFETP<ET>, L2HighOrderFE_Shape<ET>, ET, DGFiniteElement<ET_trait<ET>::DIM>>,
                          public ET_trait<ET>
  {
    enum { DIM = ET_trait<ET>::DIM };
  public:
    template <typename TA> 
    L2HighOrderFETP (int aorder, const TA & avnums, Allocator & lh)
    {
      this->order = aorder;
      for (int i = 0; i < ET_trait<ET>::N_VERTEX; i++) this->vnums[i] = avnums[i];
      this->ndof = ET_trait<ET>::PolDimension (aorder);
      if (this->vnums[0] >= this->vnums[1] ||
          this->vnums[1] >= this->vnums[2] ||
          this->vnums[1] >= this->vnums[3])
        cerr << "tensor-tet: wrong orientation" << endl;
    }

    template<typename Tx, typename TFA>  
    INLINE void T_CalcShape (TIP<ET_trait<ET>::DIM,Tx> ip, TFA & shape) const;

    virtual void ComputeNDof() { ; } 
    virtual void SetOrder (INT<DIM> p) { ; } 
    virtual void PrecomputeTrace () { ; } 
    virtual void PrecomputeGrad () { ; }

    int GetNDof1d () const { return this->order+1; }
    int GetNDof2d () const { return (this->order+1)*(this->order+2)/2; }
    int NShapeX () const { return (this->order+1)*(this->order+1); }

    template <typename IP, typename TZ>
    void CalcZFactorIP (const IP & z, const TZ & facz) const
    {
      auto hz = 2*z-1;
      if (this->vnums[2] >= this->vnums[3]) hz = -hz;
      LegendrePolynomial(this->order, hz, facz);
    }

    /*
    void CalcZFactor(const SIMD_IntegrationRule & irz, FlatMatrix<SIMD<double>> simd_facz) const
    {
      for (size_t i = 0; i < irz.Size(); i++)
        {
          auto hz = 2*irz[i](0)-1;
          if (this->vnums[2] >= this->vnums[3]) hz = -hz;
          LegendrePolynomial(this->order, hz, simd_facz.Col(i));
        }
    }
    */
    template <typename IP, typename TY>
    void CalcYFactorIP (IP y, const TY & facy) const
    {
      size_t order = this->order;
      IP hv(1.0);
      for (size_t i = 0, jj = 0; i <= order; i++)
        {
          JacobiPolynomialAlpha jac(2*i+1);
          // jac.EvalMult (order-i, 2*y-1, hv, facy.Range(jj, jj+order-i+1));
          jac.EvalMult (order-i, 2*y-1, hv, facy+jj);
          jj += order+1-i;
          hv *= (1-y);
        }
    }

    
    void CalcYFactor(const SIMD_IntegrationRule & iry, FlatMatrix<SIMD<double>> simd_facy) const
    {
      size_t order = this->order;
      for (size_t k = 0; k < iry.Size(); k++)
        {
          auto col = simd_facy.Col(k);
          SIMD<double> hv(1.0);
          SIMD<double> y = iry[k](0);
          for (size_t i = 0, jj = 0; i <= order; i++)
            {
              JacobiPolynomialAlpha jac(2*i+1);
              jac.EvalMult (order-i, 2*y-1, hv, col.Range(jj, jj+order-i+1));
              jj += order+1-i;
              hv *= (1-y);
            }
	}
    }

    void CalcYFactor(size_t i, const SIMD_IntegrationRule & iry,
                     FlatMatrix<SIMD<double>> simd_facy) const
    {
      JacobiPolynomialAlpha jac(2*i+1);
      for (size_t k = 0; k < iry.Size(); k++)
        {
          SIMD<double> hv(1.0);
          SIMD<double> y = iry[k](0);
          for (size_t j = 0; j < i; j++)
            hv *= (1-y);
          jac.EvalMult (this->order-i, 2*y-1, hv, simd_facy.Col(k));
	}
    }

    IntRange Map1t2 (size_t nr1d) const
    {
      // 0 -> 0                     = 0 * (order+1.5)
      // 1 -> order+1               = 1 * (order+1)
      // 2 -> order+1 + order       = 2 * (order+0.5)
      size_t order = this->order;
      size_t first = nr1d * (2*order-nr1d+3) / 2;
      size_t next = first + order+1-nr1d;
      return IntRange (first, next);
    }

    template <typename IP, typename TX>
    void CalcXFactorIP (IP x, const TX & facx) const
    {
      IP hv(1.0);
      for (int i = 0, jj = 0; i <= this->order; i++)
        {
          JacobiPolynomialAlpha jac(2*i+2);
          // jac.EvalMult (this->order-i, 2*x-1, hv, facx.Range(jj, jj+this->order+1));
          jac.EvalMult (this->order-i, 2*x-1, hv, facx+jj);
          jj += this->order+1;
          hv *= (1-x);
        }
    }

    
    void CalcXFactor(const SIMD_IntegrationRule & irx, FlatMatrix<SIMD<double>> simd_facx) const
    {
      for (size_t k = 0; k < irx.Size(); k++)
        {
          auto col = simd_facx.Col(k);
          SIMD<double> hv(1.0);
          SIMD<double> x = irx[k](0);
          for (int i = 0, jj = 0; i <= this->order; i++)
            {
              JacobiPolynomialAlpha jac(2*i+2);
              jac.EvalMult (this->order-i, 2*x-1, hv, col.Range(jj, jj+this->order+1));
              jj += this->order+1;
              hv *= (1-x);
            }
	}
    }

    template <typename FUNC>
    void Map1t2(FUNC f) const
    {
      size_t order = this->order;
      size_t inc = order+1, base = 0;
      for (size_t i = 0; i <= order; i++, inc--)
        {
          size_t next = base+inc;
          f(i, IntRange(base, next));
          base = next;
        }
    }

    template <int ORDER, typename FUNC>
    INLINE void Map2t3FO(FUNC f) const
    {
      for (size_t i = 0, ii = 0, jj = 0; i <= ORDER; i++)
        for (size_t j = 0; j <= ORDER-i; j++, jj++)
          {
            f(INT<4, size_t> (ii, jj, (i+j)*(ORDER+1), ORDER+1-i-j)); // base 3, base 2, base x, nr
            ii += ORDER+1-i-j;
          }
    }
    
    
    template <typename FUNC>
    INLINE void Map2t3(FUNC f) const
    {
      size_t order = this->order;
      /*
      switch (order)
        {
        case 0: Map2t3FO<0> (f); return;
        case 1: Map2t3FO<1> (f); return;
        case 2: Map2t3FO<2> (f); return;
        case 3: Map2t3FO<3> (f); return;
        case 4: Map2t3FO<4> (f); return;
        case 5: Map2t3FO<5> (f); return;
        case 6: Map2t3FO<6> (f); return;
        default: break;
        }
      */
      for (size_t i = 0, ii = 0, jj = 0; i <= order; i++)
        for (size_t j = 0; j <= order-i; j++, jj++)
          {
            f(INT<4, size_t> (ii, jj, (i+j)*(order+1), this->order+1-i-j)); // base 3, base 2, base x, nr
            ii += order+1-i-j;
          }
    }

    
  };
}
