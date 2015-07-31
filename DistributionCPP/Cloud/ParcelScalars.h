#ifndef __PARCELSCALARS_H__
#define __PARCELSCALARS_H__

struct ParcelScalars {
  // Scalars needed at initialization for parcel cloud
  int particles_;
  int parcels_;
  double Rmean_;
  double Tmean_;
  double Xmin_;
  double Xmax_;
  double Ymin_;
  double Ymax_;
  int maxiter_;
  int refreshRate_;
  int SplashFlag_;
  int TrackSplashFlag_;

};

#endif
