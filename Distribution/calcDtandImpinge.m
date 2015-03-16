function calcDtandImpinge(cloud,airfoil,fluid)
% Function which does the following:
% (1) calculate which cell is occupied by all droplets
% (2) set global time step based on cell volumes
% (3) determine and track which droplets have impinged on the airfoil

x = fluid.x; y = fluid.y; NS = fluid.NS;
dt = [];t = cloud.time;
if cloud.FLAGtimeResolve==1
    % Find parcels which are have already entered the injection domain
    tGLOB = cloud.tGLOB;
    indT = find(t<=tGLOB);
    cloud.indT = indT;
else
    % Consider all particles simultaneously
    indT = [1:cloud.particles]';
    cloud.indT = indT;
end
% Find parcels which have already impinged/stuck to airfoil and except them
% Also except parcels past the airfoil
indStick = cloud.impingeTotal;
indAdv = setdiff(indT,indStick);
indPast = find(cloud.x>=1);
indAdv = setdiff(indAdv,indPast);
cloud.indAdv = indAdv;
if ~isempty(indAdv)
    % Nearest neighbor grid point search over those particles currently in the
    % simulation which have not impinged/stuck to airfoil
    xq = cloud.x(indAdv); yq = cloud.y(indAdv); 
    uq = cloud.u(indAdv); vq = cloud.v(indAdv);
    particles = size(xq,1);
    ind = knnsearch(NS,[xq,yq]);
    % Set timesteps based on cell areas
    velMag = sqrt(uq.^2 + vq.^2);
    [~,~,nx,ny,~,~] = airfoil.findPanel(xq,yq);
    normvel = uq.*nx + vq.*ny;
    area = fluid.cellarea(ind);
    dt = 0.2*sqrt(area)./velMag;
    % Check to see if any points are on the airfoil surface
    n = size(x,1)-1;
    surfaceFlag1 = ind-2*n<=0;
    surfaceFlag2 = normvel<0.01;
    surfaceFlag = surfaceFlag1 & surfaceFlag2;
    indSurf = indAdv(find(surfaceFlag==1));
    % If past airfoil, set dt=0
    dt(xq>1) = 0;
    % Set currently impinging indices (only retaining new impingements)
    set(cloud,'impinge',[]);
    set(cloud,'impinge',indSurf);

    if cloud.FLAGtimeResolve==1
        % Set global timestep as minimum of dt
        dtNZ = dt(dt~=0);
        if isempty(dtNZ)
        % If we are in 'dead region' between next impinging particle, fast forward
        % in time at a specified rate while advecting no particles
            dt = zeros(length(indAdv),1);
            dtINTER = 0.01;
            set(cloud,'tGLOB',cloud.tGLOB+dtINTER);
        else
            dt(:) = min(dtNZ);
        end
    end
    set(cloud,'dt',dt);
else
    set(cloud,'impinge',[]);
    set(cloud,'dt',[]);
end

end