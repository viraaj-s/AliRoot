// --- ROOT system
#include <TFile.h>
#include <TClonesArray.h>
#include <TList.h>
#include <TObjString.h>
#include <TTimeStamp.h>

#include "AliZDCPreprocessor.h"
#include "AliCDBManager.h"
#include "AliCDBEntry.h"
#include "AliCDBMetaData.h"
#include "AliDCSValue.h"
#include "AliAlignObj.h"
#include "AliAlignObjParams.h"
#include "AliLog.h"
#include "AliZDCDataDCS.h"
#include "AliZDCCalibData.h"

/////////////////////////////////////////////////////////////////////
//								   //
// Class implementing ZDC pre-processor.			   //
// It takes data from DCS and passes it to the class AliZDCDataDCS //
// The class is then written to the CDB.			   //
//								   //
/////////////////////////////////////////////////////////////////////

ClassImp(AliZDCPreprocessor)

//______________________________________________________________________________________________
AliZDCPreprocessor::AliZDCPreprocessor(AliShuttleInterface* shuttle) :
  AliPreprocessor("ZDC", shuttle),
  fData(0)
{
  // constructor
}

//______________________________________________________________________________________________
AliZDCPreprocessor::~AliZDCPreprocessor()
{
  // destructor
}


//______________________________________________________________________________________________
void AliZDCPreprocessor::Initialize(Int_t run, UInt_t startTime,
	UInt_t endTime)
{
  // Creates AliZDCDataDCS object

  AliPreprocessor::Initialize(run, startTime, endTime);

	Log(Form("\n\tRun %d \n\tStartTime %s \n\tEndTime %s", run,
		TTimeStamp(startTime).AsString(),
		TTimeStamp(endTime).AsString()));

 	fRun = run;
        fStartTime = startTime;
        fEndTime = endTime;

	fData = new AliZDCDataDCS(fRun, fStartTime, fEndTime);
}

//______________________________________________________________________________________________
UInt_t AliZDCPreprocessor::Process(TMap* dcsAliasMap)
{
  // *************** From DCS ******************
  // Fills data into a AliZDCDataDCS object
  if(!dcsAliasMap) return 1;

  // The processing of the DCS input data is forwarded to AliZDCDataDCS
  Float_t dcsValues[28]; // DCSAliases=28
  fData->ProcessData(*dcsAliasMap, dcsValues);
  //dcsAliasMap->Print("");
  //
  // --- Writing ZDC table positions into alignment object
  TClonesArray *array = new TClonesArray("AliAlignObjParams",10);
  TClonesArray &alobj = *array;
  AliAlignObjParams a;
  Double_t dx=0., dz=0., dpsi=0., dtheta=0., dphi=0.;
  // Vertical table position in mm from DCS
  Double_t dyZN1 = (Double_t) (dcsValues[0]/10.);
  Double_t dyZP1 = (Double_t) (dcsValues[1]/10.);
  Double_t dyZN2 = (Double_t) (dcsValues[2]/10.);
  Double_t dyZP2 = (Double_t) (dcsValues[3]/10.);
  const char *n1ZDC="ZDC/NeutronZDC1";
  const char *p1ZDC="ZDC/ProtonZDC1";
  const char *n2ZDC="ZDC/NeutronZDC2";
  const char *p2ZDC="ZDC/ProtonZDC2";
  UShort_t iIndex=0;
  AliGeomManager::ELayerID iLayer = AliGeomManager::kInvalidLayer;
  UShort_t volid = AliGeomManager::LayerToVolUID(iLayer,iIndex);
  //
  new(alobj[0]) AliAlignObjParams(n1ZDC, volid, dx, dyZN1, dz, dpsi, dtheta, dphi, kTRUE);
  new(alobj[1]) AliAlignObjParams(p1ZDC, volid, dx, dyZP1, dz, dpsi, dtheta, dphi, kTRUE);
  new(alobj[2]) AliAlignObjParams(n2ZDC, volid, dx, dyZN2, dz, dpsi, dtheta, dphi, kTRUE);
  new(alobj[3]) AliAlignObjParams(p2ZDC, volid, dx, dyZP2, dz, dpsi, dtheta, dphi, kTRUE);
  // save in CDB storage
  AliCDBMetaData md;
  md.SetResponsible("Chiara Oppedisano");
  md.SetComment("Alignment object for ZDC");
  Bool_t resultAl = kFALSE;
  resultAl = Store("Align","Data", array, &md, 0, 0);
  
  AliZDCCalibData *calibdata = new AliZDCCalibData("ZDC");
  
// *************** From DAQ ******************
// *****************************************************
// [a] PEDESTALS -> Pedestal subtraction
// *****************************************************
TString runType = GetRunType();
printf("\n\t AliZDCPreprocessor -> runType detected %s\n\n",runType.Data());
if(runType == "PEDESTAL_RUN"){
  TList* daqSources = GetFileSources(kDAQ, "PEDESTALS");
  if(!daqSources){
    Log(Form("No source for PEDESTALS run %d !", fRun));
    return 1;
  }
  Log("\t List of sources for PEDESTALS");
  daqSources->Print();
  //
  TIter iter(daqSources);
  TObjString* source = 0;
  Int_t i=0;
  while((source = dynamic_cast<TObjString*> (iter.Next()))){
       Log(Form("\n\t Getting file #%d\n",++i));
       TString stringPedFileName = GetFile(kDAQ, "PEDESTALS", source->GetName());
       if(stringPedFileName.Length() <= 0){
          Log(Form("No PEDESTAL file from source %s!", source->GetName()));
	  return 1;
       }
       const char* pedFileName = stringPedFileName.Data();
       // no. ADCch = (22 signal ch. + 2 reference PMs) * 2 gain chain = 48
       const Int_t knZDCch = 48;
       if(pedFileName){
         FILE *file;
         if((file = fopen(pedFileName,"r")) == NULL){
           printf("Cannot open file %s \n",pedFileName);
	   return 1;
         }
         Log(Form("File %s connected to process pedestal data", pedFileName));
         Float_t pedVal[(3*knZDCch)][2];
         for(Int_t i=0; i<(3*knZDCch); i++){
            for(Int_t j=0; j<2; j++){
               fscanf(file,"%f",&pedVal[i][j]);
	       //if(j==1) printf("pedVal[%d] -> %f, %f \n",i,pedVal[i][0],pedVal[i][1]);
            }
            if(i<knZDCch){
              calibdata->SetMeanPed(i,pedVal[i][0]);
              calibdata->SetMeanPedWidth(i,pedVal[i][1]);
            }
            else if(i>=knZDCch && i<(2*knZDCch)){
              calibdata->SetOOTPed(i-knZDCch,pedVal[i][0]);
              calibdata->SetOOTPedWidth(i-knZDCch,pedVal[i][1]);
            }
            else if(i>=(2*knZDCch) && i<(3*knZDCch)){
              calibdata->SetPedCorrCoeff(i-(2*knZDCch),pedVal[i][0],pedVal[i][1]);
            }
         }
       }
       else{
          Log(Form("File %s not found", pedFileName));
          return 1;
       }
       //
      //calibdata->Print("");
  }
  delete daqSources; daqSources = 0;
}
// *****************************************************
// [b] EMD EVENTS -> Energy calibration and equalization
// *****************************************************
else if(runType == "PULSER_RUN"){
  TList* daqSources = GetFileSources(kDAQ, "EMDCALIB");
  if(!daqSources){
    AliError(Form("No sources for PULSER_RUN run %d !", fRun));
    return 1;
  }
  Log("\t List of sources for PULSER_RUN");
  daqSources->Print();
  //
  TIter iter2(daqSources);
  TObjString* source = 0;
  Int_t j=0;
  while((source = dynamic_cast<TObjString*> (iter2.Next()))){
       Log(Form("\n\t Getting file #%d\n",++j));
       TString stringEMDFileName = GetFile(kDAQ, "EMDCALIB", source->GetName());
       if(stringEMDFileName.Length() <= 0){
         Log(Form("No EMDCALIB file from source %s!", source->GetName()));
	 return 1;
       }
       const char* emdFileName = stringEMDFileName.Data();
       if(emdFileName){
    	 FILE *file;
    	 if((file = fopen(emdFileName,"r")) == NULL){
    	   printf("Cannot open file %s \n",emdFileName);
	   return 1;
    	 }
    	 Log(Form("File %s connected to process data from EM dissociation events", emdFileName));
    	 //
	 Float_t fitValEMD[6]; Float_t equalCoeff[5][4];
	 Float_t calibVal[4];
    	 for(Int_t j=0; j<10; j++){	    
    	   if(j<6){
	     fscanf(file,"%f",&fitValEMD[j]);
             if(j<4){
	       calibVal[j] = fitValEMD[j]/2.76;
	       calibdata->SetEnCalib(j,calibVal[j]);
	     }
	     else calibdata->SetEnCalib(j,fitValEMD[j]);
	   }
	   else{
	     for(Int_t k=0; k<5; k++){
	        fscanf(file,"%f",&equalCoeff[j][k]);
	        if(j==6) calibdata->SetZN1EqualCoeff(k, equalCoeff[j][k]);
	 	else if(j==7) calibdata->SetZP1EqualCoeff(k, equalCoeff[j][k]);
	 	else if(j==8) calibdata->SetZN2EqualCoeff(k, equalCoeff[j][k]);
	 	else if(j==9) calibdata->SetZP2EqualCoeff(k, equalCoeff[j][k]);	 
             }
	   }
         }
       }
       else{
         Log(Form("File %s not found", emdFileName));
         return 1;
       }
       //calibdata->Print("");
  }
}
// ********************************************************
// [c] PHYSICS RUNS -> Parameters needed for reconstruction
// ********************************************************
else if(runType == "PHYSICS"){
  TList* daqSources = GetFileSources(kDAQ, "PHYSICS");
  if(!daqSources){
    AliError(Form("No sources for PHYSICS run %d !", fRun));
    return 1;
  }
  Log("\t List of sources for PHYSICS");
  daqSources->Print();
  //
  TIter iter2(daqSources);
  TObjString* source = 0;
  Int_t j=0;
  while((source = dynamic_cast<TObjString*> (iter2.Next()))){
       Log(Form("\n\t Getting file #%d\n",++j));
       TString stringPHYSFileName = GetFile(kDAQ, "PHYSICS", source->GetName());
       if(stringPHYSFileName.Length() <= 0){
         Log(Form("No PHYSICS file from source %s!", source->GetName()));
	 return 1;
       }
       const char* physFileName = stringPHYSFileName.Data();
       if(physFileName){
    	 FILE *file;
    	 if((file = fopen(physFileName,"r")) == NULL){
    	   printf("Cannot open file %s \n",physFileName);
	   return 1;
    	 }
    	 Log(Form("File %s connected to process data from PHYSICS runs", physFileName));
    	 //
	 Float_t physRecParam[10]; 
    	 for(Int_t j=0; j<10; j++) fscanf(file,"%f",&physRecParam[j]);
	 calibdata->SetZEMEndValue(physRecParam[0]);
	 calibdata->SetZEMCutFraction(physRecParam[1]);
	 calibdata->SetDZEMSup(physRecParam[2]);
	 calibdata->SetDZEMInf(physRecParam[3]);
	 calibdata->SetEZN1MaxValue(physRecParam[4]);
	 calibdata->SetEZP1MaxValue(physRecParam[5]);
	 calibdata->SetEZDC1MaxValue(physRecParam[6]);
	 calibdata->SetEZN2MaxValue(physRecParam[7]);
	 calibdata->SetEZP2MaxValue(physRecParam[8]);
	 calibdata->SetEZDC2MaxValue(physRecParam[9]);
       }
       else{
         Log(Form("File %s not found", physFileName));
         return 1;
       }
       //calibdata->Print("");
  }
} 
else {
  Log(Form("Nothing to do: run type is %s", runType.Data()));
  return 0;
} 

  // note that the parameters are returned as character strings!
  const char* nEvents = GetRunParameter("totalEvents");
  if(nEvents) Log(Form("Number of events for run %d: %s",fRun, nEvents));
  else Log(Form("Number of events not put in logbook!"));

  // Storing the final CDB file
  AliCDBMetaData metaData;
  metaData.SetBeamPeriod(0);
  metaData.SetResponsible("Chiara");
  metaData.SetComment("Filling AliZDCCalibData object");

  Bool_t resultCal = kFALSE;
  resultCal = Store("Calib","Data",calibdata, &metaData, 0, 1);
 
  UInt_t result = 0;
  if(resultAl == kFALSE || resultCal == kFALSE){
    if(resultAl == kFALSE  && resultCal == kFALSE ) result = 3;
    else result = 2;
  }
  
  return result;
  
}
