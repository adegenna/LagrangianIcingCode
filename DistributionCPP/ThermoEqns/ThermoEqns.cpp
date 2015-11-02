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

ThermoEqns::ThermoEqns(const char* filenameCHCF, const char* filenameBETA, Airfoil& airfoil, FluidScalars& fluid, const char* strSurf) {
  // Constructor to read in input files and initialize thermo eqns

  strSurf_ = strSurf;
  NPts_ = 1000;
  // Set rhoL_,muL_
  muL_ = 1.787e-3;
  // Set LWC_,Uinf_
  LWC_ = 0.55e-3;
  Uinf_ = 100;
  // TEST: set values of other parameters
  Td_ = -10.0;
  cW_ = 4217.6;     // J/(kg C) at T = 0 C and P = 100 kPa
  ud_ = 80.0;
  cICE_ = 2093.0;   // J/(kg C) at T = 0
  Lfus_ = 334774.0; // J/kg
  // Read in values of fluid parameters
  TINF_ = 250.0; // K
  rhoL_ = fluid.rhol_;
  rhoINF_ = (3.302857142857084e-05)*pow(TINF_,2) + (-0.022130857142857)*TINF_ + 4.875685714285685; // O(2) fit between T=[175,275]
  pINF_ = rhoINF_*287.058*TINF_;
  // Initial guess for ice accretion
  mice_.resize(NPts_);
  for (int i=0; i<NPts_; i++)
    mice_[i] = 0.0;
  this->interpUpperSurface(filenameCHCF,airfoil,"CHCF");
  this->interpUpperSurface(filenameBETA,airfoil,"BETA");
  // Flip things if we are doing the lower surface
  if (strcmp(strSurf_,"LOWER")==0) {
    s_ = flipud(s_);
    s_ = -1*s_;
    cH_ = flipud(cH_);
    cF_ = flipud(cF_);
    beta_ = flipud(beta_);
  }

}

void ThermoEqns::interpUpperSurface(const char* filename, Airfoil& airfoil, const char* parameter) {
  // Function to interpolate upper surface

  MatrixXd data; VectorXd s; 
  VectorXd beta;
  VectorXd ch; VectorXd cf;
  int indFirst, indLast, indMinCF;
  double stagPt;
  // Determine if we are doing the upper or lower airfoil surface (w.r.t. stagPt)
  double s_min,s_max;
  if (strcmp(strSurf_,"UPPER")==0) {
    s_min = 0.0;
    s_max = 0.4;
  }
  else if (strcmp(strSurf_,"LOWER")==0) {
    s_min = -0.4;
    s_max = 0.0;
  }
  if (strcmp(parameter,"CHCF") == 0) {
    // Import (s,ch,cf)
    data = this->readCHCF(filename);
    s = data.col(0);
    ch = data.col(1);
    cf = data.col(2);
    // Scale CH
    for (int i=0; i<ch.size(); i++)
      ch(i) = -1.0*rhoINF_*pow(pINF_/rhoINF_,1.5)/(273.15-TINF_)*ch(i);
    // Set stagPt at s=0
    stagPt = airfoil.getStagPt();
    // Compensate for slight misalignment of stagPt by finding where cF is approx 0
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
    for (int i=0; i<CFtmp.size(); i++) {
      if (strcmp(strSurf_,"UPPER")==0)
	CFtmp[i] = cf(indFirst+i-5);
      else if (strcmp(strSurf_,"LOWER")==0)
	CFtmp[i] = cf(indLast+i-5);
    }
    minCF = min(CFtmp,indMinCF);
    if (strcmp(strSurf_,"UPPER")==0) {
      indFirst += indMinCF-5;
      stagPt = s(indFirst);
    }
    else if (strcmp(strSurf_,"LOWER")==0) {
      indLast += indMinCF-5;
      stagPt = s(indLast);
    }
    // Center s-coords about the stagnation point
    for (int i=0; i<s.size(); i++) {
      s(i) -= stagPt;
    }
    // Refine surface grid
    s_.resize(NPts_);
    double ds = (s(indLast)-s(indFirst))/NPts_;
    for (int i=0; i<NPts_; i++) {
      s_[i] = s(indFirst) + i*ds;
    }
    // Interpolate parameter values on grids
    int NPts_orig = indLast-indFirst+1;
    vector<double> s_orig(NPts_orig);
    vector<double> ch_orig(NPts_orig);
    vector<double> cf_orig(NPts_orig);
    vector<double> beta_orig(NPts_orig);
    for (int i=0; i<NPts_orig; i++) {
      s_orig[i] = s(indFirst+i);
      ch_orig[i] = ch(indFirst+i);
      cf_orig[i] = cf(indFirst+i);
    }
    gsl_interp_accel *acc = gsl_interp_accel_alloc();
    cH_.resize(NPts_);
    cF_.resize(NPts_);
    gsl_spline *splineCH = gsl_spline_alloc(gsl_interp_linear, NPts_orig);
    gsl_spline *splineCF = gsl_spline_alloc(gsl_interp_linear, NPts_orig);
    gsl_spline_init(splineCH, &s_orig[0], &ch_orig[0], NPts_orig);
    gsl_spline_init(splineCF, &s_orig[0], &cf_orig[0], NPts_orig);
    for (int i=0; i<NPts_; i++) {
      if ((s_[i] >= s_orig[0]) && (s_[i] <= s_orig[NPts_orig-1])) { 
	cH_[i] = gsl_spline_eval(splineCH, s_[i], acc);
        cF_[i] = (0.5*rhoINF_*pow(Uinf_,2)*ds)*gsl_spline_eval(splineCF, s_[i], acc);
      }
      else {
	cH_[i] = 0.0;
        cF_[i] = 0.0;
      }
    }
  }
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
      SLast[i] = s(i) - s_max;
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
      else
	beta_[i] = 0.0;
    }
  }

}

ThermoEqns::~ThermoEqns() {

}

MatrixXd ThermoEqns::readCHCF(const char* filenameCHCF) {
  // Function to read in skin friction coefficient from file

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
  // Resize cF matrix
  MatrixXd s_cH_cF(sizeCHCF,3);
  double a,b,d;
  for (int i=0; i<sizeCHCF; i++) {
    fscanf(filept,"%lf %lf %lf",&a,&b,&d);
    s_cH_cF(i,0) = a; s_cH_cF(i,1) = b; s_cH_cF(i,2) = d;
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
    fscanf(filept,"%lf,%lf",&a,&b);
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
  for (int i=0; i<x.size(); i++) {
    F[i] = (0.5/muL_)*pow(x[i],2)*cF_[i];
  }
  // Calculate fluxes at cell faces (Roe scheme upwinding)
  for (int i=0; i<x.size()-1; i++) {
    xFACE[i] = 0.5*(x[i]+x[i+1]);
    cfFACE[i] = 0.5*(cF_[i]+cF_[i+1]);
    DF[i] = (1/muL_)*(xFACE[i]*cfFACE[i]);
    f[i] = 0.5*(F[i]+F[i+1]) - 0.5*std::abs(DF[i])*(x[i+1]-x[i]);
  }
  // Calculate error for internal cells
  double ds,mimp;
  for (int i=1; i<x.size()-1; i++) {
    ds = s_[i+1]-s_[i];
    mimp = beta_[i]*LWC_*Uinf_;
    D_flux[i-1] = f[i]-f[i-1];
    I_sources[i-1] = (1./rhoL_)*ds*(mimp-mice_[i]);
    err[i] = D_flux[i-1] - I_sources[i-1];
  }
  // Boundary conditions
  err[0] = x[0]-0;
  err[x.size()-1] = err[x.size()-2];

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
  // Calculate body centered fluxes
  for (int i=0; i<NPts_; i++)
    F[i] = (0.5*cW_/muL_)*pow(x[i],2)*Y[i]*cF_[i];
  // Calculate fluxes at cell faces (Roe scheme upwinding)
  for (int i=0; i<NPts_-1; i++) {
    xFACE[i] = 0.5*(x[i]+x[i+1]);
    cfFACE[i] = 0.5*(cF_[i]+cF_[i+1]);
    DF[i] = (cW_/2.0/muL_)*cfFACE[i]*pow(xFACE[i],2);
    f[i] = 0.5*(F[i]+F[i+1]) - 0.5*std::abs(DF[i])*(Y[i+1]-Y[i]);
  }
  // Calculate error for internal cells
  double ds,mimp,RHS;
  for (int i=1; i<NPts_-1; i++) {
    ds = s_[i+1]-s_[i];
    mimp = beta_[i]*LWC_*Uinf_;
    D_flux[i-1] = f[i]-f[i-1];
    RHS = (1./rhoL_)*(mimp*(cW_*Td_ + 0.5*pow(ud_,2)) + z[i]*(Lfus_ - cICE_*Y[i]) + cH_[i]*(Td_ - Y[i]));
    I_sources[i-1] = ds*RHS;
    err[i] = D_flux[i-1] - I_sources[i-1];
  }
  // Boundary conditions
  double maxZ = 0.0;
  for (int i=0; i<NPts_; i++) {
    if (z[i]>maxZ)
      maxZ = z[i];
  }
  if (z[0] < 0.01*maxZ)
    err[0] = Y[0] - Td_;
  else
    err[0] = Y[0] - 0.0;
  err[NPts_-1] = err[NPts_-2];

  return err;

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

  vector<double> Z(NPts_);
  vector<double> X = s_;
  vector<double> I(NPts_);
  double mimp;
  C_filmHeight = true;
  for (int i=0; i<NPts_; i++) {
    mimp = beta_[i]*LWC_*Uinf_;
    I[i] = mimp - mice_[i];
  }
  vector<double> INT = trapz(X,I);
  for (int i=0; i<NPts_; i++) {
    if ((INT[i] >= 0) && (cF_[i] != 0))
      Z[i] = sqrt((2.0*muL_/rhoL_/cF_[i])*INT[i]);
    else {
      // Set film height to zero
      Z[i] = 0.0;
      C_filmHeight = false;
    }
  }

  return Z;
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
  // Implementation using finite volume with Roe scheme calculation of fluxes
  // Calculate body centered fluxes
  F = (0.5*cW_/muL_)*X*X*Y*cF_;
  // Calculate fluxes at cell faces
  for (int i=0; i<NPts_-1; i++) {
    xFACE[i] = 0.5*(X[i]+X[i+1]);
    sFACE[i] = 0.5*(s_[i]+s_[i+1]);
    cfFACE[i] = 0.5*(cF_[i]+cF_[i+1]);
  }
  DF = (cW_/2/muL_)*cF_*xFACE*xFACE;
  for (int i=0; i<NPts_-1; i++)
    f[i] = 0.5*(F[i]+F[i+1]) - 0.5*std::abs(DF[i])*(Y[i+1]-Y[i]);
  // Solve discretization for ice accretion rate
  for (int i=1; i<NPts_-1; i++) {
    D_flux = f[i]-f[i-1];
    dsFACE = sFACE[i]-sFACE[i-1];
    mimp = beta_[i]*Uinf_*LWC_;
    RHS = mimp*(cW_*Td_ + 0.5*pow(ud_,2)) + cH_[i]*(Td_ - Y[i]);
    Z[i] = ((rhoL_/dsFACE)*D_flux - RHS)/(Lfus_ - cICE_*Y[i]);
  }
  Z[0] = Z[1];
  Z[NPts_-1] = Z[NPts_-2];

  return Z;
}

vector<double> ThermoEqns::explicitSolver(const char* balance, vector<double>& y0, double eps, double tol) {
  // Function to explicitly drive mass/energy balance to steady state
  
  int iter = 1;
  double ERR = 1.0;
  vector<double> DY(y0.size());
  vector<double> Y = y0;
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
  vector<double> absVec = abs(DY);
  double ERR0 = max(absVec);
  vector<double> err;
  // Iteratively drive balance to steady state
  while ((ERR > tol*ERR0) && (ERR > 1.0e-10) && (iter < 50000)) {
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
	if (Y[i]<0)
	  Y[i] = 0;
      }
    }
    // Get error
    absVec = abs(DY);
    ERR = max(absVec);
    err.push_back(ERR);
  }
  // If still not converged, try increasing step size
  if (iter == 50000) {
    eps = 10*eps;
    while ((ERR > tol*ERR0) && (ERR > 1.0e-10) && (iter < 75000)) {
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
	  if (Y[i]<0)
	    Y[i] = 0;
	}
      }
      // Get error
      absVec = abs(DY);
      ERR = max(absVec);
      err.push_back(ERR);
    }
  }
  //printf("Explicit solver converged after %d iterations\n",iter);

  return Y;
}



void ThermoEqns::SolveIcingEqns() {
  // Main subroutine to iteratively solve mass/energy equations

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
  double dXthermo = (10.0e-3)/999;
  for (int i=0; i<Xthermo.size(); i++) {
    //Xthermo[i] = i*dXthermo;
    Xthermo[i] = 1.e-3;
    Ythermo[i] = 0.0;
    Zthermo[i] = 0.0;
  }
  setHF(Xthermo);
  setTS(Ythermo);
  setMICE(Zthermo);
  // Begin main iterative solver
  double epsEnergy = 1.0e1;
  double tolEnergy = 1.0e-4;
  for (int iterThermo = 0; iterThermo<5; iterThermo++) {
    // MASS
    printf("ITER = %d\n\n",iterThermo);
    printf("Solving mass equation...");
    Xnew = integrateMassEqn(C_filmHeight);
    printf("done.\n");
    //Xnew = explicitSolver("MASS",Xthermo,1.0e1,1.0e-6);
    //Xnew = NewtonKrylovIteration("MASS",Xthermo,1.0e-5);
    Xthermo = Xnew;
    setHF(Xthermo);
    // Constraint: check that conservation of mass is not violated
    if (C_filmHeight == false)
      printf("CONSTRAINT: Conservation of mass violated (negative film height)\n");
    for (int i=1; i<Xthermo.size(); i++) {
      if (Xthermo[i]==0.0) {
	mimp = beta_[i]*LWC_*Uinf_;
	Zthermo[i] = mimp;
      }
    }
    setMICE(Zthermo);
    // ENERGY
    printf("Solving energy equation...");
    Ynew = explicitSolver("ENERGY",Ythermo,epsEnergy,tolEnergy);
    printf("done.\n");
    //Ynew = NewtonKrylovIteration("ENERGY",Ythermo,0.06);
    Ythermo = Ynew;
    setTS(Ythermo);
    // CONSTRAINTS
    indWaterSize = 0;
    for (int i=0; i<XY.size(); i++) {
      XY[i] = Xthermo[i]*Ythermo[i];
      if (XY[i] < 1000*epsWater) {
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
    // Ice cannot be warm
    if (indIceSize == 0)
      C_iceCold = true;
    else {
      printf("CONSTRAINT: Ice above freezing detected.\n");
      // If we have warm ice, cool it down using epsIce
      C_iceCold = false;
      for (int i=0; i<indIce.size(); i++)
	Zthermo[indIce[i]] = epsIce/Ythermo[indIce[i]];
      // Correct for ice rate < 0
      for (int i=0; i<Zthermo.size(); i++) {
	if (Zthermo[i]<0)
	  Zthermo[i] = 0.0;
      }
      setMICE(Zthermo);
    }
    // Water cannot be cold
    if (indWaterSize == 0)
      C_waterWarm = true;
    else {
      printf("CONSTRAINT: Water below freezing detected.\n");
      // If we have freezing water, warm it up using epsWater
      C_waterWarm = false;
      for (int i=0; i<NPts_; i++)
	Ytmp[i] = Ythermo[i];
      for (int i=0; i<indWaterSize; i++)
	Ytmp[indWater[i]] = 0.0;
      // Re-solve for ice profile
      Zthermo = SolveThermoForIceRate(Xthermo,Ytmp);
      // Correct for ice rate < 0
      for (int i=0; i<Zthermo.size(); i++) {
	if (Zthermo[i]<0)
	  Zthermo[i] = 0.0;
      }
      setMICE(Zthermo);
    }
    // Check constraints
    if ((C_filmHeight == true) && (C_waterWarm == true) && (C_iceCold == true)) {
      printf("All compatibility relations satisfied.\n");
      break;
    }
  }
  // Check to see if need refinement of ice profile for mixed glaze/rime conditions
  if ((C_filmHeight == false) || (C_waterWarm == false) || (C_iceCold == false)) {
    printf("Mixed glaze/rime conditions detected, refining ice profile...\n");
    // Calculate mass surplus
    vector<double> massTotal(NPts_);
    vector<double> MIMP(NPts_);
    for (int i=0; i<NPts_; i++) {
      MIMP[i] = beta_[i]*LWC_*Uinf_;
      massTotal[i] = MIMP[i] - Zthermo[i];
    }
    vector<double> massSurplusCumSum = trapz(s_,massTotal);
    double massSurplus = massSurplusCumSum[NPts_-1];
    // Yupper = glaze ice everywhere; Ylower = rime profile everywhere
    vector<double> Yzero(s_.size());
    for (int i=0; i<NPts_; i++)
      Yzero[i] = 0.0;
    vector<double> Zupper = SolveThermoForIceRate(Xthermo,Yzero);
    vector<double> Zlower = MIMP;
    vector<double> Ztmp(NPts_);
    vector<double> Ytmp(NPts_);
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
	    massTotal[j] = MIMP[j] - Ztmp[j];
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
      if (indRAISE1[0] == 1.0)
	indRAISE.erase(indRAISE.begin());
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
	    massTotal[j] = MIMP[j] - Ztmp[j];
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
  // Mirror solution if we are doing the lower surface
  vector<double> sThermo = getS();
  if (strcmp(strSurf_,"LOWER")==0) {
    sThermo = flipud(sThermo);
    sThermo = -1*sThermo;
    Xthermo = flipud(Xthermo);
    Ythermo = flipud(Ythermo);
    Zthermo = flipud(Zthermo);
    s_ = sThermo;
    setHF(Xthermo);
    setTS(Ythermo);
    setMICE(Zthermo);
  }
  // Output everything to file
  FILE* outfile;
  const char* thermoFileName;
  if (strcmp(strSurf_,"UPPER")==0)
    thermoFileName = "THERMO_SOLN_UPPER.out";
  else if (strcmp(strSurf_,"LOWER")==0)
    thermoFileName = "THERMO_SOLN_LOWER.out";
  outfile = fopen(thermoFileName,"w");
  for (int i=0; i<NPts_; i++)
    fprintf(outfile,"%lf\t%lf\t%lf\t%lf\n",sThermo[i],Xthermo[i],Ythermo[i],Zthermo[i]);
  fclose(outfile);

}
