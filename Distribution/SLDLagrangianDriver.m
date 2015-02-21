%% Initialization

% Read in flow solution
meshfile = 'MESH.P3D';
solnfile = 'q103.0.50E+01.bin';
[~,~,~,~,~,~,mach,alpha,~,~] = readp3d(meshfile,solnfile);
% Dimensional reference quantities
pinf = 1.01325e5; % N/m^2
R = 287.058; % J/kg/K
Tinf = 300; % K
rhoinf = pinf/R/Tinf;
Ubar = sqrt(pinf/rhoinf);
rhol = 1000; % kg/m^3
Rd = 50e-6; % m
LWC = 0.4e-3; % kg/m^3
Uinf = mach*340; % m/s
% Initialize fluid object
scalars = [pinf;R;Tinf;rhoinf;Ubar;rhol];
fluid = Fluid(scalars,meshfile,solnfile);
% Initialize the airfoil surface
x = fluid.x; y = fluid.y;
ind = find(x(:,1)<=1);
ax = x(ind,1); ay = y(ind,1);
airfoil = Airfoil([ax,ay]);
% Initialize domain and associated distribution functions
strPDFTypes = {'Gaussian','Gaussian','Gaussian','Gaussian','Uniform'};
simTime = 60*10;
PDFparams = [20 2; 10 1; 50e-6 10e-6; 0 2; 0 simTime];
domain = InjectionDomain(strPDFTypes,PDFparams,fluid,airfoil,LWC,simTime);
nClumps = 100;
domain.sampleRealization(nClumps);
domain.dispSampleStatistics();

%% Collection efficiency

STATE = {};
strImpMod = 'Impingement';
[STATE,totalImpinge,impinge,s,beta] = calcCollectionEfficiency(cloud.rdAvg,airfoil,fluid,strImpMod);
airfoil.calcStagPt(fluid);
figure; plot(s-airfoil.stagPt*ones(length(s),1),beta);

%% Simulation

tic;
STATE = {};
% Create tree-search object for nearest neighbor searches
IMP = 1;
for t=1:tsteps
    % Subroutine to calculate new particles entering the domain (used if a
    % continuous stream of particles is desired)
    cloud.calcNewParticles(fluid);
    % Call subroutine to calculate local timesteps and impinging particles
    calcDtandImpinge(cloud,airfoil,fluid);
    % Advect particles one time step
    transportSLD(cloud,fluid);
    % Check for fracture
    fragmentSLD(cloud,fluid);
    % Call subroutine to calculate impingement regimes for impinging particles
    if ~isempty(cloud.impinge)
        %break;
        impingementRegimeSLD(cloud,airfoil);
    end
    % Save state variables
    STATE{t} = cloud.getState();
    t
    
end
toc

%% Visualization (Video)

indend = 1500;
indstop = floor(indend/10);
impimp = cloud.impingeTotal(cloud.impingeTotal<=particles);
impsplash = cloud.parentind;

% Colormap scheme
cmap = jet(100);
maxt = max(STATE{indend}(:,6));
GAIN = 130/maxt;
F(indstop) = struct('cdata',[],'colormap',[]);
for i=1:indstop
    ind=10*i;
    figure(10); hold on; plot(x(:,1),y(:,1),'k');
    scatter(STATE{ind}(:,1),STATE{ind}(:,2),3,'b','filled');
    scatter(STATE{ind}(impimp,1),STATE{ind}(impimp,2),3,'go');
    scatter(STATE{ind}(impsplash,1),STATE{ind}(impsplash,2),3,'ro');
    xlim([-.1 .4]); ylim([-.25 .25]); 
    F(i) = getframe;
    pause(.01); clf;
end

%% Visualization (Plot)

X = []; Y = []; CMAP = [];
for i=1:indstop
    ind = 10*i;
    X = [X; STATE{ind}(:,1)]; Y = [Y; STATE{ind}(:,2)];
    %colorind = round(GAIN*STATE{ind}(:,6));
    %padcmap = size(STATE{ind},1);
    %CMAP = [CMAP; repmat(cmap(i,:),padcmap,1)];
end
tic;
figure(1); hold on; scatter(X,Y,10,'k','filled');
hold on; plot(x(:,1),y(:,1),'b','LineWidth',3); axis equal;
xlim([-.25 1.1]); ylim([-.6 .6]);
toc
%{
indimp = cloud.impinge;
hold on; plot(cloud.x(indimp),cloud.y(indimp),'o','Color','r');

hold on; scatter(cloud.x(cloud.bounce),cloud.y(cloud.bounce),'filled','MarkerFaceColor','g');
hold on; scatter(cloud.x(cloud.spread),cloud.y(cloud.spread),'filled','MarkerFaceColor','r')
hold on; scatter(cloud.x(cloud.splash),cloud.y(cloud.splash),'filled','MarkerFaceColor','k')

% Highlight current state
X = STATE{indend}(:,1); Y = STATE{indend}(:,2);
figure(1); hold on; scatter(X,Y,10,'o','MarkerFaceColor','g','MarkerEdgeColor','g');
%}







