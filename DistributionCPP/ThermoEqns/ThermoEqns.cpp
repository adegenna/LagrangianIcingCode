#include "ThermoEqns.h"
#include <Eigen/Dense>
#include <gsl_errno.h>
#include <gsl_spline.h>
#include <GMRES/include/gmres.h>            // IML++ GMRES template
#include <stdio.h>
#include <stdlib.h>
#include <iterator>
#include <iostream>
#include <fstream>
#include <VectorOperations/VectorOperations.h>

using namespace std;
using namespace Eigen;

ThermoEqns::ThermoEqns(const std::string& inDir, const char* filenameCHCF, const char* filenameBETA, Airfoil& airfoil, FluidScalars& fluid, Cloud& cloud, PLOT3D& p3d, const char* strSurf, const char* strShot) {
  // Constructor to read in input files and initialize thermo eqns

  inDir_   = inDir;
  strSurf_ = strSurf;
  strShot_ = strShot;
  // Set rhoL_,muL_
  muL_ = 1.787e-3;
  cpAir_ = 1003.0;
  State state = cloud.getState();
  Td_ = fluid.Td_-273.15;
  // ASSUMPTION: set values of some parameters at certain temperature/pressure
  cW_ = 4217.6;     // J/(kg C) at T = 0 C and P = 100 kPa
  //cW_ = 4393.0;      // J/(kg C) at T = -20 C and P = 100 kPa
  cICE_ = 2093.0;   // J/(kg C) at T = 0 C       
  //cICE_ = 1943.0;    // J/(kg C) at T = -20 C
  Lfus_ = 334774.0; // J/kg
  // Read in values of fluid parameters
  NPts_   = fluid.NPts_;
  LWC_    = fluid.LWC_;
  Uinf_   = fluid.Uinf_;
  ud_     = Uinf_; // Could reset this to be local velocity if desired
  TINF_   = Td_+273.15; // K
  rhoL_   = fluid.rhol_;
  pINF_   = fluid.pinf_;
  rhoINF_ = pINF_/(287.058*TINF_);
  chord_  = fluid.chord_;
  Levap_  = (2500.8 - 2.36*(TINF_-273.15) + 0.0016*pow((TINF_-273.15),2) - 0.00006*pow((TINF_-273.15),3))*1000.0;
  Lsub_   = (2834.1 - 0.29*(TINF_-273.15) - 0.0040*pow((TINF_-273.15),2))*1000.0;
  mach_   = fluid.mach_;
  // Initial guess for ice accretion
  mice_.resize(NPts_);
  for (int i=0; i<NPts_; i++)
    mice_[i] = 0.0;
  // Interpolate CH,CF,BETA from files
  interpUpperSurface(filenameCHCF,airfoil,"CHCF");
  interpUpperSurface(filenameBETA,airfoil,"BETA");
  // Compute static pressure from P3D grid reference
  //computePstat(p3d);
  //interpUpperSurface(" ",airfoil,"PSTAT");

  // Flip things if we are doing the lower surface
  if (strcmp(strSurf_,"LOWER")==0) {
    s_ = flipud(s_);
    s_ = -1.0*s_;
    cH_ = flipud(cH_);
    cF_ = flipud(cF_);
    beta_ = flipud(beta_);
  }
  // Initialize some variables
  mevap_.resize(NPts_);
  D_mevap_.resize(NPts_);
  m_out_.resize(NPts_);
  ts_.resize(NPts_);
  mice_.resize(NPts_);
  for (int i=0; i<NPts_; i++) {
    mevap_[i] = 0.0;
    D_mevap_[i] = 0.0;
    m_out_[i] = 0.0;
    ts_[i] = 0.0;
    mice_[i] = 0.0;
  }

}

void ThermoEqns::interpUpperSurface(const char* filename, Airfoil& airfoil, const char* parameter) {
  // Function to interpolate upper surface

  MatrixXd data; VectorXd s; 
  VectorXd beta;
  VectorXd ch; VectorXd cf; VectorXd Te; VectorXd pstat; VectorXd Ubound;
  int indFirst, indLast, indMinCF;
  double stagPt;
  // Determine if we are doing the upper or lower airfoil surface (w.r.t. stagPt)
  double s_min,s_max;
  if (strcmp(strSurf_,"UPPER")==0) {
    s_min = 0.0;
    s_max = 0.2*chord_;
  }
  else if (strcmp(strSurf_,"LOWER")==0) {
    s_min = -0.2*chord_;
    s_max = 0.0;
  }

  // CH,CF,Tedge,Ubound
  double aINF = sqrt(1.4*287.058*TINF_);
  if (strcmp(parameter,"CHCF") == 0) {
    // Import (s,ch,cf,Ubound)
    data   = this->readCHCF(filename);
    s      = data.col(0)*chord_;
    ch     = data.col(1);
    cf     = data.col(2);
    Te     = data.col(3);
    pstat  = data.col(4);
    Ubound = data.col(5);
    // Fix s-coordinates to those used in airfoil object if we are doing single-shot
    if (strcmp(strShot_,"SINGLESHOT")==0) {
      char buf[256];
      strcpy(buf,inDir_.c_str()); strcat(buf,"/AirfoilS.out");
      FILE* fileS = fopen(buf,"r");
      assert(fileS != NULL);
      // Determine size of input file
      int c;
      int sizeS = 0;
      while ( (c=fgetc(fileS)) != EOF ) {
	if ( c == '\n' )
	  sizeS++;
      }
      rewind(fileS);
      // Read in s-coordinates
      double a;
      for (int i=0; i<sizeS; i++) {
	fscanf(fileS,"%le",&a);
	s[i] = a;
      }
      // Close file streams
      fclose(fileS);
    }
    // Set stagPt at s=0
    stagPt = airfoil.getStagPt();
    // Find relevant segment of ch/cf for interpolation
    double minCF;
    vector<double> SFirst(s.size());
    vector<double> SLast(s.size());
    vector<double> SFirstABS(s.size());
    vector<double> SLastABS(s.size());
    vector<double> CFtmp(11);
    for (int i=0; i<s.size(); i++) {
      SFirst[i] = s(i) - (stagPt+s_min);
      SLast[i] = s(i) - (stagPt+s_max);
    }
    SFirstABS = abs(SFirst);
    SLastABS = abs(SLast);
    minCF = min(SFirstABS,indFirst);
    minCF = min(SLastABS,indLast);
    // Compensate for slight misalignment of stagPt by finding where cF is approx 0
    // for (int i=0; i<CFtmp.size(); i++) {
    //   if (strcmp(strSurf_,"UPPER")==0)
    // 	CFtmp[i] = cf(indFirst+i-5);
    //   else if (strcmp(strSurf_,"LOWER")==0)
    // 	CFtmp[i] = cf(indLast+i-5);
    // }
    // minCF = min(CFtmp,indMinCF);
    // if (strcmp(strSurf_,"UPPER")==0) {
    //   indFirst += indMinCF-5;
    //   stagPt = s(indFirst);
    // }
    // else if (strcmp(strSurf_,"LOWER")==0) {
    //   indLast += indMinCF-5;
    //   stagPt = s(indLast);
    // }
    // Center s-coords about the stagnation point
    sP3D_.resize(s.size());
    for (int i=0; i<s.size(); i++) {
      s(i) -= stagPt;
      sP3D_[i] = s(i);
    }
    // Refine surface grid
    s_.resize(NPts_);
    double ds = (s(indLast)-s(indFirst))/NPts_;
    for (int i=0; i<NPts_; i++) {
      s_[i] = s(indFirst) + i*ds;
    }
    // Interpolate parameter values on grids
    indFirst_ = indFirst; indLast_ = indLast;
    int NPts_orig = indLast-indFirst+1;
    vector<double> s_orig(NPts_orig);
    vector<double> ch_orig(NPts_orig);
    vector<double> cf_orig(NPts_orig);
    vector<double> Te_orig(NPts_orig);
    vector<double> pstat_orig(NPts_orig);
    vector<double> Ubound_orig(NPts_orig);
    vector<double> beta_orig(NPts_orig);
    for (int i=0; i<NPts_orig; i++) {
      s_orig[i]       = s(indFirst+i);
      ch_orig[i]      = ch(indFirst+i);
      cf_orig[i]      = cf(indFirst+i);
      Te_orig[i]      = Te(indFirst+i);
      pstat_orig[i]   = pstat(indFirst+i);
      Ubound_orig[i]  = Ubound(indFirst+i);
    }
    gsl_interp_accel *acc = gsl_interp_accel_alloc();
    cH_.resize(NPts_);
    Qdot_.resize(NPts_);
    cF_.resize(NPts_);
    Te_.resize(NPts_);
    Trec_.resize(NPts_);
    pstat_.resize(NPts_);
    Ubound_.resize(NPts_);
    gsl_spline *splineCH     = gsl_spline_alloc(gsl_interp_linear, NPts_orig);
    gsl_spline *splineCF     = gsl_spline_alloc(gsl_interp_linear, NPts_orig);
    gsl_spline *splineTe     = gsl_spline_alloc(gsl_interp_linear, NPts_orig);
    gsl_spline *splinePstat  = gsl_spline_alloc(gsl_interp_linear, NPts_orig);
    gsl_spline *splineUbound = gsl_spline_alloc(gsl_interp_linear, NPts_orig);
    gsl_spline_init(splineCH,     &s_orig[0], &ch_orig[0],     NPts_orig);
    gsl_spline_init(splineCF,     &s_orig[0], &cf_orig[0],     NPts_orig);
    gsl_spline_init(splineTe,     &s_orig[0], &Te_orig[0],     NPts_orig);
    gsl_spline_init(splinePstat,  &s_orig[0], &pstat_orig[0],  NPts_orig);
    gsl_spline_init(splineUbound, &s_orig[0], &Ubound_orig[0], NPts_orig);
    // Interpolate values and scale appropriately
    double Trec;
    double rec = pow(0.9,0.3333); // Turbulent recovery factor (Pr = 0.9)
    for (int i=0; i<NPts_; i++) {
      if ((s_[i] >= s_orig[0]) && (s_[i] <= s_orig[NPts_orig-1])) { 
	cH_[i]     = gsl_spline_eval(splineCH, s_[i], acc);
        cF_[i]     = (0.5*rhoINF_*pow(Uinf_,2))*gsl_spline_eval(splineCF, s_[i], acc);
	Te_[i]     = gsl_spline_eval(splineTe,    s_[i], acc);
	pstat_[i]  = pINF_*gsl_spline_eval(splinePstat, s_[i], acc); 
	Ubound_[i] = sqrt(pINF_*rhoINF_)*gsl_spline_eval(splineUbound, s_[i], acc);
	// Calculate cH based on Ubound (velocity at boundary layer edge)
	Trec       = Te_[i] + rec*pow(Ubound_[i],2.0)/2.0/cpAir_;
	Trec_[i]   = Trec;
	//Qdot_[i]   = cH_[i]*rhoINF_*pow(pINF_/rhoINF_,1.5);
	cH_[i]     = cH_[i]*rhoINF_*pow(pINF_/rhoINF_,1.5)/(Trec-273.15);
      }
      else {
	cH_[i]     = 0.0;
        cF_[i]     = 0.0;
	Te_[i]     = 0.0;
	pstat_[i]  = 0.0;
	Ubound_[i] = 0.0;
      }
    }
    // Limiter on low values of cF (so that it isn't actually zero anywhere)
    double maxCF = cF_[0];
    for (int i=1; i<NPts_; i++) {
      if (cF_[i] > maxCF)
	maxCF = cF_[i];
    }
    for (int i=0; i<NPts_; i++) {
      if (std::abs(cF_[i]) < 0.01*maxCF)
	cF_[i] = 0.01*maxCF;
    }
  }

  // BETA
  else if (strcmp(parameter,"BETA") == 0) {
    // Assumes we have already imported/interpolated CHCF
    // Import (s,beta)
    data = this->readBetaXY(filename);
    s = data.col(0);
    beta = data.col(1);
    // Extract relevant segment of beta
    double minCF;
    vector<double> SFirst(s.size());
    vector<double> SLast(s.size());
    vector<double> SFirstABS(s.size());
    vector<double> SLastABS(s.size());
    vector<double> BETAtmp(11);
    for (int i=0; i<s.size(); i++) {
      SFirst[i] = s(i) - s_min;
      SLast[i]  = s(i) - s_max;
    }
    SFirstABS = abs(SFirst);
    SLastABS = abs(SLast);
    minCF = min(SFirstABS,indFirst);
    minCF = min(SLastABS,indLast);
    int NPts_orig = indLast-indFirst+1;
    vector<double> s_orig(NPts_orig);
    vector<double> beta_orig(NPts_orig);
    for (int i=0; i<NPts_orig; i++) {
      s_orig[i] = s(indFirst+i);
      beta_orig[i] = beta(indFirst+i);
    }
    // Interpolate on grid
    gsl_interp_accel *acc = gsl_interp_accel_alloc();
    beta_.resize(NPts_);
    gsl_spline *splineBETA = gsl_spline_alloc(gsl_interp_linear, NPts_orig);
    gsl_spline_init(splineBETA, &s_orig[0], &beta_orig[0], NPts_orig);
    for (int i=0; i<NPts_; i++) {
      if ((s_[i] >= s_orig[0]) && (s_[i] <= s_orig[NPts_orig-1]))
	beta_[i] = gsl_spline_eval(splineBETA, s_[i], acc);
      else if ((strcmp(strSurf_,"LOWER")==0) && (s_[i] > s_orig[NPts_orig-1]))
	beta_[i] = beta_orig[NPts_orig-1];
      else if ((strcmp(strSurf_,"UPPER")==0) && (s_[i] < s_orig[0]))
	beta_[i] = beta_orig[0];
      else
	beta_[i] = 0.0;
    }
  }

  // PSTAT
  else if (strcmp(parameter,"PSTAT") == 0) {
    // Interpolate static pressure
    
    // Get relevant portion of pstat
    int NPts_orig = indLast_-indFirst_+1;
    vector<double> s = sP3D_;
    vector<double> s_orig(NPts_orig);
    vector<double> pstat_orig(NPts_orig);
    for (int i=0; i<NPts_orig; i++) {
      s_orig[i] = s[indFirst_+i];
      pstat_orig[i] = pstat_[indFirst_+i];
    }
    // Declare/initialize interpolant
    gsl_interp_accel *acc = gsl_interp_accel_alloc();
    gsl_spline *splinePSTAT = gsl_spline_alloc(gsl_interp_linear, NPts_orig);
    gsl_spline_init(splinePSTAT, &s_orig[0], &pstat_orig[0], NPts_orig);
    // Interpolate
    pstat_.resize(NPts_);
    for (int i=0; i<NPts_; i++) {
      if ((s_[i] >= s_orig[0]) && (s_[i] <= s_orig[NPts_orig-1])) {
	// If we are within interpolation bounds, simply interpolate
	pstat_[i] = gsl_spline_eval(splinePSTAT, s_[i], acc);
      }
      else {
	// If we are outside interpolation bounds, set equal to zero
	pstat_[i] = 0.0;
      }
    }
  }
  

}

ThermoEqns::~ThermoEqns() {

}

MatrixXd ThermoEqns::readCHCF(const char* filenameCHCF) {
  // Function to read in S,CH,CF,Tedge,Pedge

  // Initialize file stream
  FILE* filept = fopen(filenameCHCF,"r");
  assert(filept != NULL);
  // Determine size of input file
  int c;
  int sizeCHCF = 0;
  while ( (c=fgetc(filept)) != EOF ) {
    if ( c == '\n' )
      sizeCHCF++;
  }
  rewind(filept);
  // Resize data matrix
  MatrixXd s_cH_cF(sizeCHCF,6);
  double a,b,d,e,f,g;
  for (int i=0; i<sizeCHCF; i++) {
    fscanf(filept,"%le %le %le %le %le %le",&a,&b,&d,&e,&f,&g);
    s_cH_cF(i,0) = a; s_cH_cF(i,1) = b; s_cH_cF(i,2) = d; 
    s_cH_cF(i,3) = e; s_cH_cF(i,4) = f; s_cH_cF(i,5) = g; 
  }
  // Close file streams
  fclose(filept);

  return s_cH_cF;

}

MatrixXd ThermoEqns::readBetaXY(const char* filenameBeta) {
  // Function to read in skin friction coefficient from file

  // Initialize file stream
  FILE* filept = fopen(filenameBeta,"r");
  assert(filept != NULL);
  // Determine size of input file
  int c;
  int sizeBeta = 0;
  while ( (c=fgetc(filept)) != EOF ) {
    if ( c == '\n' )
      sizeBeta++;
  }
  rewind(filept);
  // Resize cF matrix
  MatrixXd s_Beta(sizeBeta,2);
  double a,b;
  for (int i=0; i<sizeBeta; i++) {
    fscanf(filept,"%lf\t%lf",&a,&b);
    s_Beta(i,0) = a; s_Beta(i,1) = b;
  }
  // Close file streams
  fclose(filept);

  return s_Beta;

}

// Define action of Jacobian on vector
vector<double> ThermoEqns::JX(int func, vector<double>& X, vector<double>& u0) {
  vector<double> jx(u0.size());
  double eps = 1.e-4;
  vector<double> X2(u0.size());
  for (int i=0; i<u0.size(); i++) {
    X2[i] = u0[i] + eps*X[i];
  }
  // Determine which mass/energy balance to use
  vector<double> f1;
  vector<double> f2;
  if (func==0) {
    f2 = massBalance(X2);
    f1 = massBalance(u0);
  }
  else if (func==1) {
    f2 = energyBalance(X2);
    f1 = energyBalance(u0);
  }
  else if (func==2) {
    f2 = testBalance(X2);
    f1 = testBalance(u0);
  }
  for (int i=0; i<u0.size(); i++) {
    jx[i] = (1./eps)*(f2[i]-f1[i]);
  }
  return jx;
}

vector<double> ThermoEqns::massBalance(vector<double>& x) {
  // Function to compute mass balance

  vector<double> err(x.size());
  vector<double> F(x.size());
  vector<double> f(x.size()-1);
  vector<double> xFACE(x.size()-1);
  vector<double> cfFACE(x.size()-1);
  vector<double> DF(x.size()-1);
  vector<double> D_flux(x.size()-2);
  vector<double> I_sources(x.size()-2);
  // Calculate body centered fluxes
  for (int i=0; i<NPts_; i++) {
    F[i] = (0.5/muL_)*pow(x[i],2)*cF_[i];
  }
  // Calculate fluxes at cell faces (Roe scheme upwinding)
  double maxCF = cF_[0];
  for (int i=0; i<NPts_; i++) {
    if (cF_[i]>maxCF)
      maxCF = cF_[i];
  }
  for (int i=0; i<NPts_-1; i++) {
    xFACE[i]  = 0.5*(x[i]+x[i+1]);
    cfFACE[i] = 0.5*(cF_[i]+cF_[i+1]);
    DF[i]     = (1/muL_)*(xFACE[i]*cfFACE[i]);
    f[i]      = 0.5*(F[i]+F[i+1]) - 0.5*std::abs(DF[i])*(x[i+1]-x[i]);
  }
  // Calculate error for internal cells
  double ds,mimp;
  for (int i=1; i<x.size()-1; i++) {
    ds             = s_[i+1]-s_[i];
    mimp           = beta_[i]*LWC_*Uinf_;
    D_flux[i-1]    = f[i]-f[i-1];
    I_sources[i-1] = (1./rhoL_)*ds*(mimp-mice_[i]-mevap_[i]);
    err[i]         = D_flux[i-1] - I_sources[i-1];
  }
  // Boundary conditions
  err[0]       = x[0]-0;
  err[NPts_-1] = 2*err[NPts_-2] - err[NPts_-3]; // Extrapolation B.C.

  return err;
}

vector<double> ThermoEqns::energyBalance(vector<double>& Y) {
  // Function to compute energy balance

  vector<double> x = hf_;
  vector<double> z = mice_;
  vector<double> err(x.size());
  vector<double> F(x.size());
  vector<double> f(x.size()-1);
  vector<double> xFACE(x.size()-1);
  vector<double> cfFACE(x.size()-1);
  vector<double> DF(x.size()-1);
  vector<double> D_flux(x.size()-2);
  vector<double> I_sources(x.size()-2);
  // Calculate evaporating mass
  computeMevap(Y);
  // Calculate body centered fluxes
  for (int i=0; i<NPts_; i++)
    F[i] = (0.5*cW_/muL_)*pow(x[i],2)*Y[i]*cF_[i];
  // Calculate fluxes at cell faces (Roe scheme upwinding)
  double LIM,r,f_smooth,f_sharp;
  for (int i=0; i<NPts_-1; i++) {
    xFACE[i]  = 0.5*(x[i]+x[i+1]);
    cfFACE[i] = 0.5*(cF_[i]+cF_[i+1]);
    DF[i]     = (cW_/2.0/muL_)*cfFACE[i]*pow(xFACE[i],2);
    f[i]      = 0.5*(F[i]+F[i+1]) - 0.5*std::abs(DF[i])*(Y[i+1]-Y[i]);
  }
  // Calculate error for internal cells
  double ds,mimp,RHS,Trec,rec;
  double S_imp,S_ice,S_conv,S_evap,S_rad;
  rec = pow(0.9,0.3333); // Turbulent recovery factor (Pr = 0.9)
  for (int i=1; i<NPts_-1; i++) {
    ds             = s_[i+1]-s_[i];
    mimp           = beta_[i]*LWC_*Uinf_;
    Trec           = Te_[i] + rec*pow(Ubound_[i],2.0)/2.0/cpAir_ - 273.15;
    D_flux[i-1]    = f[i]-f[i-1];
    S_imp          = (1./rhoL_)*(mimp*(cW_*(Td_-Y[i]) + 0.5*pow(ud_,2)));
    S_ice          = (1./rhoL_)*(z[i]*(Lfus_ - cICE_*Y[i]));
    S_conv         = (-1./rhoL_)*std::abs(cH_[i]*(Trec - Y[i]));
    //S_conv         = std::max( -std::abs(Qdot_[i]) , S_conv );
    S_evap         = (1./rhoL_)*(-0.5*(Levap_ + Lsub_)*mevap_[i]);
    RHS            = S_imp + S_ice + S_conv + S_evap;
    I_sources[i-1] = ds*RHS;
    err[i]         = D_flux[i-1] - I_sources[i-1];
  }
  // Boundary conditions
  double maxZ = 0.0;
  for (int i=0; i<NPts_; i++) {
    if (z[i]>maxZ)
      maxZ = z[i];
  }
  if (z[0] < 0.01*maxZ)
    err[0] = Y[0] - (TINF_-273.15);
  else
    err[0] = Y[0] - 0.0;
  err[NPts_-1] = 2*err[NPts_-2] - err[NPts_-3]; // Extrapolation B.C.

  return err;

}

void ThermoEqns::computePstat(PLOT3D& p3d) {
  // Function to compute static pressure (on original PLOT3D) grid

  double gamma = 1.4;
  // Get flow variables from PLOT3D object
  Eigen::MatrixXf P = p3d.getP();
  int NX = p3d.getNX();
  int NY = p3d.getNY();
  // Pull out wrap corresponding to edge of boundary layer
  pstat_.resize(NX);
  for (int i=0; i<NX; i++)
    pstat_[i] = P(i,11);
  // Cut off wake
  vector<double> ptmp(384);
  for (int i=0; i<384; i++)
    ptmp[i] = pstat_[i+64];
  pstat_.clear();
  pstat_ = ptmp;

}

void ThermoEqns::computeMevap(vector<double>& Y) {
  // Function to compute evaporating/sublimating mass

  double Ts_tilda, Tinf_tilda, p_vp, p_vinf;
  double TINF_C = TINF_-273.15;
  double Hr = 0.0; // Relative humidity (between 0 and 1)
  Tinf_tilda = 72.0 + 1.8*TINF_C;
  p_vinf = 3386.0*(0.0039 + (6.8096e-6)*pow(Tinf_tilda,2) + (3.5579e-7)*pow(Tinf_tilda,3));
  for (int i=0; i<NPts_; i++) {
    if (std::isnan(Y[i]))
      Ts_tilda = 72.0;
    else
      Ts_tilda = 72.0 + 1.8*Y[i];
    p_vp = 3386.0*(0.0039 + (6.8096e-6)*pow(Ts_tilda,2) + (3.5579e-7)*pow(Ts_tilda,3));
    mevap_[i] = (0.7*cH_[i]/cpAir_)*(p_vp - Hr*p_vinf)/pstat_[i];
    mevap_[i] = std::max(mevap_[i],0.0);
    mevap_[i] = std::min(mevap_[i],beta_[i]*LWC_*Uinf_);
  }
}

void ThermoEqns::computeMevap(double& TS, int& idx) {
  // Function to compute m_evap and d(m_evap)/d(T_S) for a single point
  // For use with LEWICEbalance

  double Ts_tilda, Tinf_tilda, p_vp, p_vinf, D_Tstilda, D_pvp;
  double TINF_C = TINF_-273.15;
  double Hr = 0.0; // Relative humidity (between 0 and 1)
  Tinf_tilda = 72.0 + 1.8*TINF_C;
  p_vinf = 3386.0*(0.0039 + (6.8096e-6)*pow(Tinf_tilda,2) + (3.5579e-7)*pow(Tinf_tilda,3));
  Ts_tilda = 72.0 + 1.8*TS;
  p_vp = 3386.0*(0.0039 + (6.8096e-6)*pow(Ts_tilda,2) + (3.5579e-7)*pow(Ts_tilda,3));
  mevap_[idx] = (0.7*cH_[idx]/cpAir_)*(p_vp - Hr*p_vinf)/pstat_[idx];
  mevap_[idx] = std::max(mevap_[idx],0.0);
  D_Tstilda = 1.8;
  D_pvp = 3386.0*(2.0*(6.8096e-6)*Ts_tilda*D_Tstilda + 3.0*(3.5579e-7)*(pow(Ts_tilda,2))*D_Tstilda);
  D_mevap_[idx] = -0.5*(Levap_+Lsub_)*(0.7*cH_[idx]/cpAir_)/pstat_[idx]*D_pvp;

}

vector<double> ThermoEqns::testBalance(vector<double>& x) {
  // Test function RHS

  vector<double> RHS(3);
  RHS[0]=1.0; RHS[1]=-1.0; RHS[2]=2.0;
  vector<double> err(3);
  Eigen::MatrixXd A(3,3);
  A << 8,1,6,3,5,7,4,9,2;
  err[0] = A(0,0)*x[0] + A(0,1)*x[1] + A(0,2)*x[2] - RHS[0];
  err[1] = A(1,0)*x[0] + A(1,1)*x[1] + A(1,2)*x[2] - RHS[1];
  err[2] = A(2,0)*x[0] + A(2,1)*x[1] + A(2,2)*x[2] - RHS[2];

  return err;
}

vector<double> ThermoEqns::trapz(vector<double>& X, vector<double>& Y) {
  // Trapezoidal integration function

  vector<double> Z(X.size());
  Z[0] = 0.0;
  for (int i=1; i<X.size(); i++) {
    Z[i] = Z[i-1] + 0.5*(X[i]-X[i-1])*(Y[i]+Y[i-1]);
  }
  
  return Z;
}

vector<double> ThermoEqns::integrateMassEqn(bool& C_filmHeight) {
  // Integrate mass eqn with trapz

  vector<double> X(NPts_);
  vector<double> S = s_;
  vector<double> I(NPts_);
  double mimp;
  C_filmHeight = true;
  for (int i=0; i<NPts_; i++) {
    mimp = beta_[i]*LWC_*Uinf_;
    I[i] = mimp - mice_[i] - mevap_[i];
  }
  vector<double> INT = trapz(S,I);
  // Check conservation of mass
  for (int i=0; i<NPts_; i++) {
    if ((INT[i] >= 0) && (cF_[i] != 0))
      X[i] = sqrt((2.0*muL_/rhoL_/cF_[i])*INT[i]);
    else {
      // Set film height to zero, and set mice to mimp-mevap
      X[i] = 0.0;
      mimp = beta_[i]*LWC_*Uinf_;
      mice_[i] = std::max(mimp-mevap_[i],0.0);
      C_filmHeight = false;
    }
  }

  return X;
}


vector<double> ThermoEqns::NewtonKrylovIteration(const char* balance, vector<double>& u0, double globaltol) {
  // Function to take a balance of form f(X) = 0 and do Newton-Krylov iteration

  // Set balance flag
  int balFlag;
  if (strcmp(balance,"MASS")==0)
    balFlag = 0;
  else if (strcmp(balance,"ENERGY")==0)
    balFlag = 1;
  else if (strcmp(balance,"TEST")==0)
    balFlag = 2;

  double tol = 1.e-3;                        // Convergence tolerance
  int result, maxit = 2000, restart = 50;    // GMRES Maximum, restart iterations

  // Initialize Jacobian and RHS, solution vectors
  int stateSize = u0.size();
  vector<double> b(stateSize);
  vector<double> x(stateSize);
  vector<double> x0(stateSize);
  vector<double> dx0(stateSize);
  vector<double> jx;
  // Storage for upper Hessenberg H
  MatrixXd H(restart+1, restart);
  // Begin iteration
  int nitermax = 20;
  vector<double> globalerr;
  vector<double> r(stateSize);
  double normR,normGlob;
  // Initialize linearization point
  vector<double> un = u0;
  
  for (int i=0; i<nitermax; i++) {
    // Reset linearization point
    u0 = un;
    x = u0;
    // Compute RHS
    if (balFlag==0)
      b = massBalance(u0);
    else if (balFlag == 1)
      b = energyBalance(u0);
    else if (balFlag==2)
      b = testBalance(u0);
    b = b*-1.0;
    // Compute approximate Jacobian
    result = GMRES(this, balFlag, x, u0, b, H, restart, maxit, tol);  // Solve system
    un = u0 + x;
    // Compute global error
    if (balFlag==0)
      globalerr = massBalance(un);
    else if (balFlag == 1)
      globalerr = energyBalance(un);
    else if (balFlag==2)
      globalerr = testBalance(un);
    jx = JX(balFlag,x,u0);
    r = globalerr*-1.0 + jx*-1.0;
    normR = NORM(r);
    normGlob = NORM(globalerr);
    printf("JFNK ERROR = %lf\tGLOBAL ERROR = %lf\n",normR,normGlob);
    // Test to see if converged
    if (normGlob < globaltol) {
      break;
    }
  }
  //for (int ii=0; ii<stateSize; ii++)
  //  printf("%e\t%e\n",s_[ii],un[ii]);

  return un;
}

void ThermoEqns::setHF(vector<double>& hf) {
  hf_ = hf;
}

void ThermoEqns::setTS(vector<double>& ts) {
  ts_ = ts;
}

void ThermoEqns::setMICE(vector<double>& mice) {
  mice_ = mice;
}

vector<double> ThermoEqns::getS() {
  return s_;
}

vector<double> ThermoEqns::getMICE() {
  return mice_;
}

vector<double> ThermoEqns::SolveThermoForIceRate(vector<double>& X, vector<double>& Y) {
  // Function to solve thermo eqn for ice accretion rate

  vector<double> F(NPts_);
  vector<double> f(NPts_-1);
  double D_flux,dsFACE,mimp,RHS;
  vector<double> xFACE(NPts_-1);
  vector<double> sFACE(NPts_-1);
  vector<double> cfFACE(NPts_-1);
  vector<double> DF(NPts_-1);
  vector<double> Z(NPts_);
  // Recompute evaporating mass
  computeMevap(Y);
  // Implementation using finite volume with Roe scheme calculation of fluxes
  // Calculate body centered fluxes
  F = (0.5*cW_/muL_)*X*X*Y*cF_;
  // Calculate fluxes at cell faces
  for (int i=0; i<NPts_-1; i++) {
    xFACE[i]  = 0.5*(X[i]+X[i+1]);
    sFACE[i]  = 0.5*(s_[i]+s_[i+1]);
    cfFACE[i] = 0.5*(cF_[i]+cF_[i+1]);
  }
  DF = (cW_/2/muL_)*cfFACE*xFACE*xFACE;
  for (int i=0; i<NPts_-1; i++)
    f[i] = 0.5*(F[i]+F[i+1]) - 0.5*std::abs(DF[i])*(Y[i+1]-Y[i]);
  f[NPts_-2] = 0.0;
  // Solve discretization for ice accretion rate
  double S_imp,S_conv,S_evap;
  double rec = pow(0.9,0.3333); // Turbulent recovery factor (Pr = 0.9)
  double Trec;
  for (int i=1; i<NPts_-1; i++) {
    D_flux = f[i]-f[i-1];
    dsFACE = sFACE[i]-sFACE[i-1];
    mimp   = beta_[i]*Uinf_*LWC_;
    Trec   = Te_[i] + rec*pow(Ubound_[i],2.0)/2.0/cpAir_ - 273.15;
    S_imp  = mimp*(cW_*(Td_-Y[i]) + 0.5*pow(ud_,2));
    S_conv = -1.0*std::abs(cH_[i]*(Trec-Y[i]));
    //S_conv = std::max( -std::abs(Qdot_[i]) , S_conv );
    S_evap = -0.5*(Levap_ + Lsub_)*mevap_[i];
    RHS    = S_imp + S_conv + S_evap;
    Z[i]   = ((rhoL_/dsFACE)*D_flux - RHS)/(Lfus_ - cICE_*Y[i]);
  }
  Z[0]       = Z[1];
  Z[NPts_-1] = Z[NPts_-2];
  // Reset evaporating mass
  computeMevap(ts_);

  return Z;
}

vector<double> ThermoEqns::explicitSolver(const char* balance, vector<double>& y0, double eps, double tol) {
  // Function to explicitly drive mass/energy balance to steady state
  
  int iter = 1;
  double ERR = 1.0;
  vector<double> DY(y0.size());
  vector<double> Y = y0;
  int CEIL = 50000;
  int CEIL2 = 50000;
  double mimp;
  // Figure out which balance we are using
  int switchBal;
  if (strcmp(balance,"MASS")==0)
    switchBal = 1;
  else if (strcmp(balance,"ENERGY")==0)
    switchBal = 2;
  // Get initial quantities
  if (switchBal==1)
    DY = massBalance(Y);
  else if (switchBal==2)
    DY = energyBalance(Y);
  Y = Y - eps*DY;
  // Flag for mass balance
  if (switchBal==1) {
    for (int i=0; i<Y.size(); i++) {
      if (Y[i]<0) {
	Y[i] = 0;
	mimp = beta_[i]*LWC_*Uinf_;
	mice_[i] = mimp - mevap_[i];
      }
      Y[i] = std::min(Y[i],20.0e-6);
    }
  }
  vector<double> absVec = abs(DY);
  double ERR0 = max(absVec);
  vector<double> err;
  // Iteratively drive balance to steady state
  while ((ERR > tol*ERR0) && (ERR > 1.0e-10) && (iter < CEIL)) {
    iter++;
    // Get balance and update Y
    if (switchBal==1)
      DY = massBalance(Y);
    else if (switchBal==2)
      DY = energyBalance(Y);
    Y = Y - eps*DY;
    // Flag for mass balance
    if (switchBal==1) {
      for (int i=0; i<Y.size(); i++) {
	if (Y[i]<0) {
	  Y[i] = 0;
	  mimp = beta_[i]*LWC_*Uinf_;
	  mice_[i] = mimp - mevap_[i];
	}
	Y[i] = std::min(Y[i],20.0e-6);
      }
    }
    else {
      for (int i=0; i<NPts_; i++) {
	Y[i] = std::max(Y[i],(TINF_-273.15)*1.0);
	Y[i] = std::min(Y[i],1.0);
      }
    }
    // Get error
    absVec = abs(DY);
    ERR = max(absVec);
    err.push_back(ERR);
  }
  // If still not converged, try increasing step size
  if (iter == CEIL) {
    eps = 10*eps;
    while ((ERR > tol*ERR0) && (ERR > 1.0e-10) && (iter < CEIL2)) {
      iter++;
      // Get balance and update Y
      if (switchBal==1)
	DY = massBalance(Y);
      else if (switchBal==2)
	DY = energyBalance(Y);
      Y = Y - eps*DY;
      // Flag for mass balance
      if (switchBal==1) {
	for (int i=0; i<Y.size(); i++) {
	  if (Y[i]<0) {
	    Y[i] = 0;
	    mimp = beta_[i]*LWC_*Uinf_;
	    mice_[i] = mimp - mevap_[i];
	  }
	}
      }
      // Get error
      absVec = abs(DY);
      ERR = max(absVec);
      err.push_back(ERR);
    }
  }
  if (strcmp(balance,"MASS")==0)
    Y[NPts_-1] = Y[NPts_-2];
  printf("Explicit solver converged after %d iterations... ",iter);

  return Y;
}

void ThermoEqns::explicitSolverSimultaneous(double eps, double tol) {
  // Function to explicitly drive mass/energy balance to steady state
  // and apply constraints (all done simultaneously)
  
  int iter = 1;
  vector<double> DX(NPts_);
  vector<double> DY(NPts_);
  vector<double> Ytmp(NPts_);
  int CEIL = 50000;
  double mimp,XY,YZ;
  vector<double> absVecX;
  vector<double> absVecY;
  vector<double> err;
  vector<double>ERRDX;
  vector<double> ERRDY;
  double ERR;

  // Initialize variables
  hf_.resize(NPts_);
  ts_.resize(NPts_);
  mice_.resize(NPts_);
  for (int i=0; i<NPts_; i++) {
    hf_[i]   = 0.0;
    ts_[i]   = 0.0;
    mice_[i] = 0.0;
  }

  // Iteratively drive balance to steady state
  ERR = 1.0;
  double ERR0 = ERR;
  //while ((ERR > tol*ERR0) && (ERR > 1.0e-10) && (iter < CEIL)) {
  while (iter < CEIL) {
    iter++;
    
    // Forward step the mass/energy equations
    DX = massBalance(hf_);
    DY = energyBalance(ts_);
    hf_ = hf_ - eps*DX;
    ts_ = ts_ - eps*DY;

    // Constraints
    Ytmp = ts_;
    for (int i=0; i<NPts_; i++) {
      // Mass limits (0 <= hf <= 20e-6)
      hf_[i] = std::max(hf_[i],0.0);
      hf_[i] = std::min(hf_[i],20.0e-6);
      // Temperature limits (2*TINF <= ts <= 10.0)
      ts_[i] = std::max(ts_[i],2.0*(TINF_-273.15));
      ts_[i] = std::min(ts_[i],10.0);
      // Constraints (XY>0 && YZ<0)
      XY = hf_[i]*ts_[i];
      YZ = ts_[i]*mice_[i];
      if ((XY < 0) && (ts_[i] < -0.001))
	Ytmp[i] = 0.0;
      if ((YZ > 0.0) && (ts_[i] > 0.001))
	Ytmp[i] = 0.0;    
    }
    mice_ = SolveThermoForIceRate(hf_,Ytmp);
    // Mass limits
    for (int i=0; i<NPts_; i++) {
      if (hf_[i] < 1.0e-10)
	mice_[i] = beta_[i]*Uinf_*LWC_ - mevap_[i];
      mice_[i] = std::max(mice_[i],0.0);
    }

    // Error metric
    absVecX = abs(DX);
    absVecY = abs(DY);
    ERRDX   = trapz(s_,absVecX);
    ERRDY   = trapz(s_,absVecY);
    ERR     = std::max(ERRDX[NPts_-1],ERRDY[NPts_-1]);
    err.push_back(ERR);

    if (iter == 1)
      ERR0 = ERR;
  }
  
  if (iter < CEIL)
    printf("Explicit solver converged after %d iterations... ",iter);
  else
    printf("Explicit solver not converged after %d iterations... ", iter);

  // Mirror solution if we are doing the lower surface
  if (strcmp(strSurf_,"LOWER")==0) {
    s_     = flipud(s_);
    s_     = -1*s_;
    hf_    = flipud(hf_);
    ts_    = flipud(ts_);
    mice_  = flipud(mice_);
    cF_    = flipud(cF_);
    cH_    = flipud(cH_);
  }  

  // Output to file
  FILE* outfile;
  FILE* errorfile;
  std::string s_thermoFileName;
  std::string s_errorFileName;
  if (strcmp(strSurf_,"UPPER")==0) {
    s_thermoFileName = inDir_ + "/THERMO_SOLN_UPPER.out";
    s_errorFileName  = inDir_ + "/THERMO_UPPER_CONV.out";
  }
  else if (strcmp(strSurf_,"LOWER")==0) {
    s_thermoFileName = inDir_ + "/THERMO_SOLN_LOWER.out";
    s_errorFileName  = inDir_ + "/THERMO_LOWER_CONV.out";
  }
  outfile   = fopen(s_thermoFileName.c_str(),"w");
  errorfile = fopen(s_errorFileName.c_str(),"w");
  for (int i=0; i<NPts_; i++)
    fprintf(outfile,"%.10f\t%.10f\t%.10f\t%.10f\t%.10f\t%.10f\t%.10f\t%.10f\n",s_[i],hf_[i],ts_[i],mice_[i],mevap_[i],cF_[i],cH_[i],Trec_[i]);
  for (int i=0; i<err.size(); i++)
    fprintf(errorfile,"%.10f\n",err[i]);
  fclose(outfile);
  fclose(errorfile);

}

void ThermoEqns::LEWICEformulation(int& idx) {
  // Subroutine to solve steady-state mass/energy as LEWICE does
  // Finite-volume, marching, 1st order upwinding
  
  // Declarations/allocations
  double ds = s_[1]-s_[0];
  double S_kin, S_conv, S_evap, S_sens;
  double D_sens, D_kin, D_conv, DEDT;
  double T_new;
  double m_imp = beta_[idx]*LWC_*Uinf_;
  double m_ice, m_in, T_in;
  double E_dot;
  double rhoICE = 916.7;
  double Tinf  = TINF_-273.15;    // Celsius
  double rec   = pow(0.9,0.3333); // Turbulent recovery factor (Pr = 0.9)
  double Trec  = Te_[idx] + rec*pow(Ubound_[idx],2.0)/2.0/cpAir_ - 273.15; // Celsius
  double Tstat = Te_[idx] - 273.15; // Celsius
  // Initial guesses
  double m_out = 0.0;
  double T_S   = 0.0;
  double Nf    = 0.5;
  // Initial parameters
  if (idx == 0) {
    m_in = 0.0;
    T_in = 0.0;
  }
  else {
    m_in = m_out_[idx-1];
    T_in = ts_[idx-1];
  }

  // ***************************
  // BEGIN MARCHING SOLVER
  // ***************************

  double D_TS = 1.0;
  double eps_T = 1.0e-4;
  double T_mp = 0.0;
  int iter = 1;
  int flagMass = 0;
  int flagTotal = 0;
  while (flagTotal == 0) {
    
    // ***************************
    // ENERGY
    // ***************************

    // Calculate evaporation
    computeMevap(T_S,idx);
    if (flagMass == 1) {
      mevap_[idx] = 0;
      D_mevap_[idx] = 0;
    }
    if (mevap_[idx] == 0)
      D_mevap_[idx] = 0;
    // Calculate source terms (which are not phase dependent)
    S_kin  = m_imp*(0.5*pow(Uinf_,2));
    S_conv = cH_[idx]*(Trec - T_S);
    S_evap = -0.5*(Levap_ + Lsub_)*mevap_[idx];
    // Calculate phase dependent source terms
    if (Nf <= 0.0) {
      // No ice
      S_sens = (m_in*T_in+m_imp*Tinf)*cW_ - (m_imp+m_in)*cW_*T_S;
      D_sens = -(m_imp+m_in)*cW_;
    }
    else if ((Nf > 0.0) && (Nf < 1.0)) {
      // Mixed (glaze)
      S_sens = (m_imp+m_in)*((cICE_*T_mp*(1.0-rhoICE/rhoL_) + cICE_*eps_T + Lfus_ - cW_*T_mp)*((T_mp+eps_T-T_S)/eps_T))
	+ cW_*( m_in*T_in + m_imp*Tinf );
      D_sens = (m_imp+m_in)*(cICE_*T_mp*(1.0-rhoICE/rhoL_) + cICE_*eps_T + Lfus_ - cW_*T_mp)*(-1.0/eps_T);
    }
    else if (Nf >= 1.0) {
      // Rime
      S_sens = (m_imp+m_in)*(cICE_*T_mp*(1.0-rhoICE/rhoL_) + cICE_*(T_mp+eps_T-T_S) + Lfus_ - cW_*T_mp) 
	+ cW_*( m_in*T_in + m_imp*Tinf );
      D_sens = -(m_imp+m_in)*cICE_;
    }
    // Calculate RHS of 0 = E_dot
    E_dot = (S_kin+S_conv+S_evap+S_sens);
    // Calculate dE/dT_S (derivative of energy balance w.r.t. T_S)
    D_kin  = 0.0;
    D_conv = -cH_[idx];
    DEDT   = D_kin+D_conv+D_mevap_[idx]+D_sens;
    // Update guess
    T_new = T_S - E_dot/DEDT;
    D_TS = std::abs(T_S-T_new);
    T_S = T_new;
    Nf = (T_mp+eps_T-T_S)/eps_T;
    if (Nf < 0)
      Nf = 0;
    else if (Nf > 1)
      Nf = 1;

    // ***************************
    // MASS
    // ***************************
  
    m_ice    = Nf*(m_imp + m_in - mevap_[idx]);
    m_out    = m_in + m_imp - mevap_[idx] - m_ice;
    if (m_out < 0)
      flagMass = 1;
    else
      flagMass = 0;

    if ((D_TS < 1.0e-10) && (flagMass == 0) || (iter > 25))
      flagTotal = 1;
    iter++;
  }

  // Set final solution
  ts_[idx] = T_S;
  m_out_[idx] = m_out;
  mice_[idx] = std::max(m_ice,0.0);
  
}

void ThermoEqns::SolveLEWICEformulation() {
  // Solve LEWICEbalance over entire airfoil

  // Reset cH to values computed using integral B.L. calculations
  //integralBL_LEWICE();

  // Solve LEWICE thermodynamic formulation
  for (int i=0; i<NPts_; i++) {
    LEWICEformulation(i);
  }

  // Mirror solution if we are doing the lower surface
  vector<double> sThermo = getS();
  if (strcmp(strSurf_,"LOWER")==0) {
    s_     = flipud(s_);
    s_     = -1*s_;
    m_out_ = flipud(m_out_);
    ts_    = flipud(ts_);
    mice_  = flipud(mice_);
    cF_    = flipud(cF_);
    cH_    = flipud(cH_);
  }  

  // Output to file
  FILE* outfile;
  std::string s_thermoFileName;
  if (strcmp(strSurf_,"UPPER")==0)
    s_thermoFileName = inDir_ + "/THERMO_SOLN_UPPER.out";
  else if (strcmp(strSurf_,"LOWER")==0)
    s_thermoFileName = inDir_ + "/THERMO_SOLN_LOWER.out";
  outfile = fopen(s_thermoFileName.c_str(),"w");
  for (int i=0; i<NPts_; i++)
    fprintf(outfile,"%.10f\t%.10f\t%.10f\t%.10f\t%.10f\t%.10f\t%.10f\n",s_[i],m_out_[i],ts_[i],mice_[i],mevap_[i],cF_[i],cH_[i]);
  fclose(outfile);

}

void ThermoEqns::integralBL_LEWICE() {
  // Function to approximate CH from integral boundary layer approximations

  // *******************************
  // CRITICAL REYNOLDS NUMBER
  // *******************************

  // LEWICE empirical correlation for critical Reynolds number
  vector<double> Rec(NPts_);
  double RecLIM = 600.0;
  double RecCorr;
  for (int i=0; i<NPts_; i++) {
    RecCorr = 3834.2 - (1.9846e5)*s_[i] + (3.2812e6)*pow(s_[i],2) - (6.9994e6)*pow(s_[i],3);
    Rec[i] = std::max(RecCorr,RecLIM);
  }
  Rec[0] = 1.0e22;
  
  // *******************************
  // ROUGHNESS REYNOLDS NUMBER
  // *******************************

  // LEWICE empirical correlation for surface roughness
  double ks  = 0.55e-3;
  double Nf  = 1.0;
  double xks = 0.5*sqrt(0.15 + 0.3/Nf);
  ks         = xks*ks;
  // Aerodynamic parameters
  double k_air  = 0.0243;         // Thermal conductivity of air at 0 C
  double mu_air = 1.725e-5;       // Viscosity of air at 0 C
  double nu_air = mu_air/rhoINF_; // Kinematic viscosity
  // Flow derivatives
  vector<double> U(NPts_);
  vector<double> DU(NPts_);
  vector<double> D2U(NPts_);
  double ds = s_[1]-s_[0];
  U = movingAverage(Ubound_,6);
  DU[0] = (U[1]-U[0])/ds; DU[NPts_-1] = (U[NPts_-1]-U[NPts_-2])/ds;
  for (int i=1; i<NPts_-1; i++) {
    DU[i]  = std::max( (U[i+1]-U[i-1])/(2*ds), 0.0 );
    if (DU[i] == 0.0)
      D2U[i] = 0.0;
    else
      D2U[i] = ( U[i+1] - 2*U[i] + U[i-1] )/pow(ds,2);
  }
  D2U[0] = D2U[1]; D2U[NPts_-1] = D2U[NPts_-2];
  // Allocation for shape factors Z, K, and Gamma
  vector<double> Z(NPts_); Z[0] = 0.0;
  vector<double> G(NPts_); G[0] = 0.0;
  vector<double> K(NPts_); K[0] = 0.0;
  double Gk0,Gk1,Gk2,Gk3;
  double Sk0,Sk1,Sk2,Sk3;
  double Uk0,Uk1,Uk2,Uk3;
  double k1,k2,k3,k4;
  vector<double> gam(NPts_);
  vector<double> kgam(NPts_);
  double h = 1.0*ds;
  // Create interpolation table for Gamma = g(K)
  for (int i=0; i<NPts_; i++) {
    gam[i]  = -12.0 + 24.0/(NPts_-1)*i;
    kgam[i] = pow((37.0/315.0 - gam[i]/945.0 - pow(gam[i],2)/9072.0),2)*gam[i];
  }
  gsl_interp_accel *acc  = gsl_interp_accel_alloc();
  gsl_spline *splineGAM  = gsl_spline_alloc(gsl_interp_linear, NPts_);
  gsl_spline_init(splineGAM, &kgam[0], &gam[0], NPts_);
  // Create interpolation table for U = u(S)
  gsl_spline *splineU = gsl_spline_alloc(gsl_interp_linear, NPts_);
  gsl_spline_init(splineU, &s_[0], &U[0], NPts_);
  // Solve for shape factors Z, K, and Gamma (Runge-Kutta integration)
  for (int i=0; i<NPts_-1; i++) {
    Gk0 = G[i]; Sk0 = s_[i]; Uk0 = gsl_spline_eval(splineU, Sk0, acc);

    k1 = dZ_dS(Gk0,Uk0); Gk1 = Gk0 + h/2*k1; Sk1 = Sk0 + h/2; Uk1 = gsl_spline_eval(splineU,Sk1,acc);
    k2 = dZ_dS(Gk1,Uk1); Gk2 = Gk0 + h/2*k2; Sk2 = Sk0 + h/2; Uk2 = gsl_spline_eval(splineU,Sk2,acc);
    k3 = dZ_dS(Gk2,Uk2); Gk3 = Gk0 + h*k3;   Sk3 = Sk0 + h;   Uk3 = gsl_spline_eval(splineU,Sk3,acc);
    k4 = dZ_dS(Gk3,Uk3);

    Z[i+1] = Z[i] + h/6*(k1 + 2*k2 + 2*k3 + k4);
    K[i+1] = Z[i+1]*DU[i+1];
    if ( (K[i+1] > kgam[0]) && (K[i+1] < kgam[NPts_-1]) )
      G[i+1] = gsl_spline_eval(splineGAM,K[i+1],acc);
    else if (K[i+1] < kgam[0])
      G[i+1] = -12.0;
    else if (K[i+1] > kgam[NPts_-1])
      G[i+1] = 12.0;
  }
  // Calculate displacement thickness of boundary layer
  vector<double> d(NPts_);
  for (int i=0; i<NPts_; i++) {
    if (DU[i] > 0)
      d[i] = sqrt(nu_air*G[i]/DU[i]);
    else if ((DU[i] == 0) && (i > 2))
      d[i] = d[i-1] + (d[i-1]-d[i-2]);
    else
      d[i] = 0.0;
  }
  // Calculate velocity profile at height of roughness element
  vector<double> Rek(NPts_);
  vector<double> Uk(NPts_);
  double n;
  for (int i=0; i<NPts_; i++) {
    n      = std::min( ks/d[i], 1.0 );
    Uk[i]  = U[i]*(2*n - 2*pow(n,3) + pow(n,4)) + 1.0/6.0*G[i]*n*pow(1-n,3);
    Rek[i] = Uk[i]*ks/nu_air;
  }
  // Calculate laminar --> turbulent transition point
  int idxT;
  for (int i=0; i<NPts_; i++) {
    if (Rek[i] > Rec[i]) {
      idxT = i;
      break;
    }
  }

  // *******************************
  // HEAT TRANSFER
  // *******************************

  cH_.resize(NPts_);
  double Pr = cpAir_*mu_air/k_air;
  // Laminar
  vector<double> I_A(NPts_);
  vector<double> A(NPts_);
  vector<double> B(NPts_);
  vector<double> C(NPts_);
  double chL,dT,dT_stag;
  for (int i=0; i<NPts_; i++) {
    I_A[i] = pow(U[i]/Uinf_,1.87)/chord_;
    B[i]   = 46.72/pow(U[i]/Uinf_,2.87);
    C[i]   = 16.28/std::abs(chord_/Uinf_*DU[i]);
  }
  A = trapz(s_,I_A);
  for (int i=0; i<NPts_; i++) {
    dT      = sqrt((nu_air*chord_/Uinf_)*B[i]*A[i]);
    dT_stag = sqrt((nu_air*chord_/Uinf_)*C[i]);
    chL = 2*k_air/(dT+dT_stag);
    if (i < idxT)
      cH_[i] = chL;
  }
  if (idxT > 6) {
    for (int i=0; i<5; i++) {
      cH_[i] = cH_[5];
    }
  }
  // Turbulent
  double Rekt, Beta, cf;
  for (int i=idxT; i<NPts_; i++) {
    Rekt   = ks*Uk[i]/nu_air;
    Beta   = 0.52*pow(Rekt,0.45)*pow(Pr,0.8);
    cf     = cF_[i]/(0.5*rhoINF_*pow(Uinf_,2)); // cF_ is actually shear stress, so rescale
    cH_[i] = 0.5*rhoINF_*cpAir_*cf*U[i]/(0.9 + sqrt(cf/2)*Beta);
  }
  
}

double ThermoEqns::dZ_dS(double G, double V) {
  // Subroutine needed for LEWICE integral B.L. calculation

  double A = 37.0/315.0 - G/945.0 - pow(G,2)/9072.0;
  double B = 2.0 - 116.0/315.0*G + (2.0/945.0 + 1.0/120.0)*pow(G,2) + 2.0/9072.0*pow(G,3);
  double F = 2*A*B;
  // Calculate dZ/ds
  double eps = 0.1;
  double sgnV = 1.0;
  if (V<0)
    sgnV = -1.0;
  if (std::abs(V) < eps)
    V = eps*sgnV;
  double DZ = F/V;

  return DZ;

}

vector<double> ThermoEqns::movingAverage(vector<double>& X, double smooth) {
  // Subroutine to perform a moving average

  int N = X.size();
  vector<double> X_smooth(N);
  double NX_smooth,NY_smooth;
  int startIND = (int) smooth/2;
  int endIND   = N - startIND;
  for (int i=0; i<N; i++) {
    X_smooth[i] = 0.0;
    if ((i > startIND) && (i < endIND)) {
      for (int j=0; j<smooth+1; j++) {
  	X_smooth[i] += X[i+j-startIND]/(smooth+1);
      }
    }
    else
      X_smooth[i] = X[i];
  }

  return X_smooth;
}

void ThermoEqns::SolveIcingEqns() {
  // Main subroutine to iteratively solve mass/energy equations
  // For use with Roe-scheme PDE solver (i.e. not LEWICE)

  vector<double> Xthermo(NPts_);
  vector<double> Ythermo(NPts_);
  vector<double> Zthermo(NPts_);
  vector<double> XY(NPts_);
  vector<double> YZ(NPts_);
  vector<double> Ytmp(NPts_);
  vector<int> indWater;
  vector<int> indIce;
  int indWaterSize, indIceSize;
  vector<double> Xnew(NPts_);
  vector<double> Ynew(NPts_);
  double epsWater = -1.0e-8;
  double epsIce = 1.0e-8;
  bool C_filmHeight, C_waterWarm, C_iceCold;
  double mimp;
  // Initial guess for X and Y (hf and Ts)
  double dXthermo = (10.0e-3)/NPts_;
  for (int i=0; i<Xthermo.size(); i++) {
    Xthermo[i] = 0.0;
    Ythermo[i] = 0.0;
    Zthermo[i] = 0.0;
  }
  setHF(Xthermo);
  setTS(Ythermo);
  setMICE(Zthermo);

  // ***************************
  // BEGIN ITERATIVE SOLVER
  // ***************************

  double epsEnergy = 2.0e1;
  double tolEnergy = 1.0e-4;
  iterSolver_ = 0;
  int iterations = 5;
  for (int iterThermo = 0; iterThermo<iterations; iterThermo++) {
    printf("ITER = %d\n\n",iterThermo+1);
    //for (int i=0; i<NPts_; i++) {
      //printf("%lf\t%lf\t%lf\t%lf\n",s_[i],mevap_[i],Ythermo[i],beta_[i]);
      //}

    // ***************************
    // MASS BALANCE
    // ***************************
    
    printf("Solving mass equation..."); fflush(stdout);
    //Xnew = integrateMassEqn(C_filmHeight);
    Xnew = explicitSolver("MASS",Xthermo,5.0e-1,1.0e-6); // eps_water = 5e-1 works
    //Xnew = NewtonKrylovIteration("MASS",Xthermo,1.0e-5);
    C_filmHeight = true;
    for (int i=0; i<NPts_; i++) {
      if (Xnew[i] < 0.01e-6) {
    	Xnew[i] = 0.0;
    	C_filmHeight = false;
      }
    }
    printf("done.\n");
    Xthermo = Xnew;
    setHF(Xthermo);

    // ***************************
    // ENERGY BALANCE
    // ***************************

    printf("Solving energy equation..."); fflush(stdout);
    Ynew = explicitSolver("ENERGY",Ythermo,1.0e1,tolEnergy); // eps_energy = 2e1 works
    // for (int i=0; i<Ynew.size(); i++) {
    //   if (std::isnan(Ynew[i]))
    // 	Ynew[i] = 0.0;
    // }
    printf("done.\n");
    //Ynew = NewtonKrylovIteration("ENERGY",Ythermo,0.06);
    Ythermo = Ynew;
    setTS(Ythermo);
    
    // ***************************
    // CONSTRAINTS
    // ***************************
    
    // Constraint: check that conservation of mass is not violated
    // if (C_filmHeight == false)
    //   printf("CONSTRAINT: Conservation of mass violated (negative film height)\n");
    // for (int i=1; i<Xthermo.size(); i++) {
    //   if (Xthermo[i]<1.0e-9) {
    // 	mimp = beta_[i]*LWC_*Uinf_;
    // 	Zthermo[i] = std::max(mimp-mevap_[i],0.0);
    //   }
    // }
    // setMICE(Zthermo);

    // Check XY and YZ
    indWaterSize = 0;
    for (int i=0; i<XY.size(); i++) {
      XY[i] = Xthermo[i]*Ythermo[i];
      if ((XY[i] < 0) && (Ythermo[i] < -0.001)) {
	indWater.push_back(i);
	indWaterSize++;
      }
    }
    indIceSize = 0;
    for (int i=0; i<Ythermo.size(); i++) {
      YZ[i] = Ythermo[i]*Zthermo[i];
      if (YZ[i] > 1000*epsIce) {
	indIce.push_back(i);
	indIceSize++;
      }
    }
    for (int i=0; i<NPts_; i++)
      Ytmp[i] = Ythermo[i];

    // Ice cannot be warm
    if (indIceSize == 0)
      C_iceCold = true;
    else {
      printf("CONSTRAINT: Ice above freezing detected.\n");
      C_iceCold = false;
      for (int i=0; i<indIce.size(); i++) {
        Ytmp[indIce[i]] = 0.0;
      }
    }

    // Water cannot be cold
    if (indWaterSize == 0)
      C_waterWarm = true;
    else {
      printf("CONSTRAINT: Water below freezing detected.\n");
      C_waterWarm = false;
      for (int i=0; i<indWaterSize; i++)
	Ytmp[indWater[i]] = 0.0;
    }

    // Re-solve for ice profile
    Zthermo = SolveThermoForIceRate(Xthermo,Ytmp);
    // Correct for ice rate < 0
    for (int i=0; i<Zthermo.size(); i++) {
      if (Zthermo[i]<0)
	Zthermo[i] = 0.0;
      Zthermo[i] = std::min(Zthermo[i], 3.0*Uinf_*LWC_); // Limiter on m_ice
    }
    if (iterThermo < iterations-1)
      setMICE(Zthermo);

    // Check constraints
    if ((C_filmHeight == true) && (C_waterWarm == true) && (C_iceCold == true)) {
      printf("All compatibility relations satisfied.\n");
      break;
    }
    iterSolver_ = iterThermo;
  }
  
  // Fix Zthermo if needed
  for (int i=1; i<NPts_; i++) {
    if (Xthermo[i] < 0.01e-6)
      Zthermo[i] = std::max(beta_[i]*LWC_*Uinf_ - mevap_[i], 0.0);
  }
  setMICE(Zthermo);
  // ***************************
  // OUTPUT SOLUTION
  // ***************************

  // vector<double> sTMP = getS();
  // FILE* outfileTMP;
  // outfileTMP = fopen("UncorrectedSoln.out","w");
  // for (int i=0; i<NPts_; i++)
  //   fprintf(outfileTMP,"%.10f\t%.10f\t%.10f\t%.10f\t%.10f\t%.10f\n",sTMP[i],Xthermo[i],Ythermo[i],Zthermo[i],cF_[i],cH_[i]);
  // fclose(outfileTMP);

  // ********************************
  // CHECK FOR INCREMENTAL REFINEMENT
  // ********************************
  /****
  // Check to see if need refinement of ice profile for mixed glaze/rime conditions
  if ((C_filmHeight == false) || (C_waterWarm == false) || (C_iceCold == false)) {
    printf("Mixed glaze/rime conditions detected, refining ice profile...\n");
    // Calculate mass surplus
    vector<double> massTotal(NPts_);
    vector<double> MIMP(NPts_);
    for (int i=0; i<NPts_; i++) {
      MIMP[i] = beta_[i]*LWC_*Uinf_;
      massTotal[i] = MIMP[i] - Zthermo[i] - mevap_[i];
    }
    vector<double> massSurplusCumSum = trapz(s_,massTotal);
    double massSurplus = massSurplusCumSum[NPts_-1];
    // Yupper = glaze ice everywhere; Ylower = rime profile everywhere
    vector<double> Yzero(s_.size());
    for (int i=0; i<NPts_; i++)
      Yzero[i] = 0.0;
    vector<double> Zupper = SolveThermoForIceRate(Xthermo,Yzero);
    vector<double> Zlower = MIMP;
    vector<double> Ztmp = Zthermo;
    bool flag,flag1,flag2;
    int indSTMP;
    if (massSurplus<0) {
      // Water mass deficit (too much ice)
      // Iteratively convert glaze to rime accretion, starting at glaze/rime interface and marching forward
      vector<double> indLOWER = find(Zthermo,Zupper,flag); 
      if (flag == true) {
	for (int i=indLOWER.size()-1; i>-1; i--) {
	  indSTMP = indLOWER[i];
	  Ztmp = Zthermo;
	  Ytmp = Ythermo;
	  for (int j=indSTMP; j<NPts_; j++) {
	    Ztmp[j] = Zlower[j];
	  }
	  for (int j=0; j<NPts_; j++)
	    massTotal[j] = MIMP[j] - Ztmp[j] - mevap_[i];
	  massSurplusCumSum = trapz(s_,massTotal);
	  massSurplus = massSurplusCumSum[NPts_-1];
	  if (massSurplus>=0) {
	    Xthermo = integrateMassEqn(C_filmHeight);
	    for (int j=indSTMP; j<NPts_; j++)
	      Xthermo[j] = 0.0;
	    setHF(Xthermo);
	    setMICE(Ztmp);
	    Ytmp = explicitSolver("ENERGY",Ythermo,epsEnergy,tolEnergy);
	    setTS(Ytmp);
	    break;
	  }
	}	
      }
    }
    else {
      // Water mass surplus (too much water)
      // Iteratively convert rime to glaze accretion, starting at rime/glaze interface and marching aft
      vector<double> indRAISE1 = find(Zthermo,Zlower,flag1);
      vector<double> indRAISE2;
      for (int i=0; i<NPts_; i++) {
	if (Zthermo[i] <= 1.0e-4) {
	  flag2 = true;
	  indRAISE2.push_back(i);
	}
      }
      vector<double> indRAISE = indRAISE1;
      if (flag2 == true) {
	for (int i=0; i<indRAISE2.size(); i++)
	  indRAISE.push_back(indRAISE2[i]);
      }
      if (flag1==true) {
	if (indRAISE1[0] == 1.0)
	  indRAISE.erase(indRAISE.begin());
      }
      if ((flag1==true) || (flag2==true)) {
	for (int i=0; i<indRAISE.size(); i++) {
	  indSTMP = indRAISE[i];
	  Ztmp = Zthermo;
	  Ytmp = Ythermo;
	  for (int j=1; j<indSTMP; j++) {
	    Ztmp[j] = Zupper[j];
	    Ytmp[j] = 0.0;
	  }
	  for (int j=0; j<NPts_; j++)
	    massTotal[j] = MIMP[j] - Ztmp[j] - mevap_[i];
	  massSurplusCumSum = trapz(s_,massTotal);
	  massSurplus = massSurplusCumSum[NPts_-1];
	  if (massSurplus<=0) {
	    Xthermo = integrateMassEqn(C_filmHeight);
	    break;
	  }
	}
      }
    }
    // Accept final solution
    setMICE(Ztmp); Zthermo = Ztmp;
    setTS(Ytmp); Ythermo = Ytmp;
    setHF(Xthermo);
    
  }
  ****/

  // Mirror solution if we are doing the lower surface
  vector<double> sThermo = getS();
  if (strcmp(strSurf_,"LOWER")==0) {
    sThermo = flipud(sThermo);
    sThermo = -1*sThermo;
    Xthermo = flipud(Xthermo);
    Ythermo = flipud(Ythermo);
    Zthermo = flipud(Zthermo);
    cF_     = flipud(cF_);
    cH_     = flipud(cH_);
    s_ = sThermo;
    setHF(Xthermo);
    setTS(Ythermo);
    setMICE(Zthermo);
  }
  // Check boundary condition of ice for bug
  int ind1,ind2;
  if (strcmp(strSurf_,"UPPER")==0) {
    for (int i=0; i<15; i++) {
      if ((Zthermo[i] < 1.0e-4) && (Zthermo[15] > 1.0e-4)) {
	Zthermo[i] = Zthermo[15];
      }
      setMICE(Zthermo);
    }
  }
  else {
     for (int i=NPts_; i>NPts_-15; i--) {
      if ((Zthermo[i] < 1.0e-4) && (Zthermo[NPts_-15] > 1.0e-4)) {
	Zthermo[i] = Zthermo[NPts_-15];
      }
      setMICE(Zthermo);
    }
  }

  // ***************************
  // OUTPUT FINAL SOLUTION
  // ***************************

  FILE* outfile;
  std::string s_thermoFileName;
  if (strcmp(strSurf_,"UPPER")==0)
    s_thermoFileName = inDir_ + "/THERMO_SOLN_UPPER.out";
  else if (strcmp(strSurf_,"LOWER")==0)
    s_thermoFileName = inDir_ + "/THERMO_SOLN_LOWER.out";
  outfile = fopen(s_thermoFileName.c_str(),"w");
  for (int i=0; i<NPts_; i++)
    fprintf(outfile,"%.10f\t%.10f\t%.10f\t%.10f\t%.10f\t%.10f\t%.10f\t%.10f\n",sThermo[i],Xthermo[i],Ythermo[i],Zthermo[i],mevap_[i],cF_[i],cH_[i],Trec_[i]);
  fclose(outfile);

}
