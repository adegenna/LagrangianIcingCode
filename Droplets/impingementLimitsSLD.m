function yL = impingementLimitsSLD(rdAvg,fluid,airfoil,xL,Ymiss,Yhit,limstr)
% FUNCTION: determine impingement limits on airfoil surface given a
% specified flow field
% INPUTS: Average rd, fluid, airfoil, xL, Ymiss, Yhit, limstr
% OUTPUTS: upper and lower limits of impingement in s-coordinates and the
% corresponding x,y coordinates

% Initial test positions
Xnew = xL; Ynew = Ymiss;
yL = Ynew;

[pg,ug,vg] = fluid.interpFluid(Xnew,Ynew);
x = fluid.x; y = fluid.y; rhol = fluid.rhol; 
Rd = rdAvg;
Unew = ug + 0.01*ug*unifrnd(-1,1);
Vnew = vg + 0.01*vg*unifrnd(-1,1);
dSpacingAvg = 1; dtSpacingAvg = 1; % This is irrelevant for our calculations here
cloud = SLDcloud([Xnew Ynew Unew Vnew Rd 0 1],rhol,1,dSpacingAvg,dtSpacingAvg);

% Iterative procedure to find upper limit
TOL = 1e-4;
maxiter = 2000; iter = 1;
STATE = [cloud.x, cloud.y];
figure(1); plot(x(:,1),y(:,1),'k'); axis equal; xlim([-.5,.2]); ylim([-.3,.3]);
while abs(Ymiss-Yhit)>TOL
    % Call subroutine to calculate local timesteps and impinging particles
    calcDtandImpinge(cloud,airfoil,fluid);
    % Advect particles one time step
    transportSLD(cloud,fluid);
    % Update field quantities at new particle positions
    Xnew = cloud.x; Ynew = cloud.y;
    STATE(iter+1,:) = [Xnew,Ynew];
    if ~isempty(cloud.impinge)
        % If impinged, stop transporting and update particle
        Yhit = yL; hitLocx = cloud.x; hitLocy = cloud.y;
        clear cloud;
        Xnew = xL; Ynew = 0.5*(Ymiss+Yhit);
        
        [pg,ug,vg] = fluid.interpFluid(Xnew,Ynew);
        Unew = ug + 0.01*ug*unifrnd(-1,1);
        Vnew = vg + 0.01*vg*unifrnd(-1,1);
        cloud = SLDcloud([Xnew Ynew Unew Vnew Rd 0 1],rhol,1,dSpacingAvg,dtSpacingAvg);
        
        yL = Ynew;
        iter=1;
        figure(1); hold on; scatter(STATE(:,1),STATE(:,2),5,'filled');
        STATE = [Xnew,Ynew];
    elseif isempty(cloud.impinge) && iter>maxiter
        % If not impinged after maxiter iterations, try another guess
        clear cloud;
        Ymiss = yL;
        Xnew = xL; Ynew = 0.5*(Ymiss+Yhit);
        
        [pg,ug,vg] = fluid.interpFluid(Xnew,Ynew);
        Unew = ug + 0.01*ug*unifrnd(-1,1);
        Vnew = vg + 0.01*vg*unifrnd(-1,1);
        cloud = SLDcloud([Xnew Ynew Unew Vnew Rd 0 1],rhol,1,dSpacingAvg,dtSpacingAvg);
        
        yL = Ynew;
        iter=1;
        figure(1); hold on; scatter(STATE(:,1),STATE(:,2),5,'filled');
        STATE = [Xnew,Ynew];
    end
    iter = iter+1;

end

% Output result to airfoil and determine s-coordinates of limit
airfoil.setLim(hitLocx,hitLocy,limstr);











end