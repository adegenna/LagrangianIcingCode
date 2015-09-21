function zn = SolveThermoForIceRate(x,y,scalars)
% Solve thermo eqn for temperature

s = scalars.s_;
ds = scalars.ds_;
pw = scalars.pw_;
uw = scalars.uw_;
cw = scalars.cw_;
Td = scalars.Td_;
ud = scalars.ud_;
cice = scalars.cice_;
Lfus = scalars.Lfus_;
ch = scalars.ch_;
mimp = scalars.mimp_;

XY = x.^2.*y;
DXY = zeros(length(x),1);
DXY(2:end-1) = (XY(3:end)-XY(1:end-2))./(s(3:end)-s(1:end-2));
DXY(1) = (XY(2)-XY(1))./(s(2)-s(1));
DXY(end) = (XY(end)-XY(end-1))./(s(end)-s(end-1));

LHS = (pw*cw/2/uw)*DXY;
RHS = mimp.*(cw*Td + 0.5*ud^2) + ch*(Td - y);

zn = (LHS-RHS)./(Lfus - cice*y);

end