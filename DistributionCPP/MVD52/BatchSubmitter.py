from pylab import *
import sys,os
import numpy
import re
import pylab
import numpy as np
import shutil
import fileinput
from subprocess import call

Basedir = "/home/adegenna/LagrangianIcingCode/DistributionCPP/";
InputRadiusFile = Basedir + "MVD52/MVD52Distribution.dat";
InputFile = Basedir + "InputData/Input.dat";
PBSFile = Basedir + "MVD52/pbsbase.dat";
numSims = 27;

RDist = genfromtxt(InputRadiusFile, delimiter = ',');
radius = RDist[:,0];
for i in range(0,numSims):
    # Create working directory, copy default files into it
    workdir = Basedir + "MVD52/workdir." + str(i+1) + "/";
    os.mkdir(workdir);
    shutil.copy2(InputFile, workdir);
    shutil.copy2(PBSFile, workdir);
    # Replace radius value in default input file
    R = radius[i];
    inputfile = workdir + "Input.dat";
    f = open(inputfile,'r'); filedata = f.read(); f.close()
    newdata = filedata.replace("RADIUS",str(R))
    f = open(inputfile,'w'); f.write(newdata); f.close()
    # Modify local pbs script
    pbsfile = workdir + "pbsbase.dat";
    f = open(pbsfile,'a');
    f.write("cd " + workdir + "\n");
    f.write(Basedir + "IcingDriver " + inputfile);
    f.close();
    # Submit job
    os.system("qsub " + "-o " + workdir + "OUT.out" + " -e" + workdir + "ERR.err " + pbsfile);
    
