/**************************************************************************
 * Copyright(c) 1998-1999, ALICE Experiment at CERN, All rights reserved. *
 *                                                                        *
 * Author: The ALICE Off-line Project.                                    *
 * Contributors are mentioned in the code where appropriate.              *
 *                                                                        *
 * Permission to use, copy, modify and distribute this software and its   *
 * documentation strictly for non-commercial purposes is hereby granted   *
 * without fee, provided that the above copyright notice appears in all   *
 * copies and that both the copyright notice and this permission notice   *
 * appear in the supporting documentation. The authors make no claims     *
 * about the suitability of this software for any purpose. It is          *
 * provided "as is" without express or implied warranty.                  *
 **************************************************************************/

/* $Id$ */

///////////////////////////////////////////////////////////////////////////////
//                                                                           //
//  This is the class for perfoming the monitoring process.                  //
//  It checks if a raw data file exists, loops over the events in the raw    //
//  data file, reconstructs TPC and ITS clusters and tracks, fills the       //
//  monitor histograms and sends the updated histograms to the clients.      //
//  Then the raw data file is deleted and it waits for a new file.           //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////


#include "AliMonitorProcess.h"
#include "AliMonitorTPC.h"
#include "AliMonitorITS.h"
#include "AliMonitorV0s.h"
#include "AliRawReaderRoot.h"
#include "AliLoader.h"
#include "AliRun.h"
#include "AliTPC.h"
#include "AliTPCclustererMI.h"
#include "AliTPCtrackerMI.h"
#include "AliITS.h"
#include "AliITSclustererV2.h"
#include "AliITStrackerV2.h"
#include "AliITSLoader.h"
#include "AliV0vertexer.h"
#include <TSystem.h>
#include <TSocket.h>
#include <TMessage.h>
#include <TGridResult.h>
#include <TROOT.h>


ClassImp(AliMonitorProcess) 


const Int_t AliMonitorProcess::kgPort = 9327;


//_____________________________________________________________________________
AliMonitorProcess::AliMonitorProcess(const char* alienDir,
				     const char* fileNameGalice)
{
// initialize the monitoring process and the monitor histograms

  fGrid = TGrid::Connect("alien", gSystem->Getenv("USER"));
  if (!fGrid || fGrid->IsZombie() || !fGrid->IsConnected()) {
    delete fGrid;
    Fatal("AliMonitorProcess", "could not connect to alien");
  }
  fGrid->cd(alienDir);
  fLogicalFileName = "";
  fFileName = "";

  fRunLoader = AliRunLoader::Open(fileNameGalice);
  if (!fRunLoader) Fatal("AliMonitorProcess", 
			 "could not get run loader from file %s", 
			 fileNameGalice);

  fRunLoader->CdGAFile();
  fTPCParam = AliTPC::LoadTPCParam(gFile);
  if (!fTPCParam) Fatal("AliMonitorProcess", "could not load TPC parameters");

  fRunLoader->LoadgAlice();
  gAlice = fRunLoader->GetAliRun();
  if (!gAlice) Fatal("AliMonitorProcess", "no gAlice object found");
  AliITS* ITS = (AliITS*) gAlice->GetModule("ITS");
  if (!ITS) Fatal("AliMonitorProcess", "no ITS detector found");
  fITSgeom = ITS->GetITSgeom();
  if (!fITSgeom) Fatal("AliMonitorProcess", "could not load ITS geometry");

  fRunNumber = 0;
  fSubRunNumber = 0;
  fNEvents = 0;
  fNEventsMin = 2;
  fWriteHistoList = kFALSE;

  fTopFolder = new TFolder("Monitor", "monitor histograms");
  fTopFolder->SetOwner(kTRUE);

  fMonitors.Add(new AliMonitorTPC(fTPCParam));
  fMonitors.Add(new AliMonitorITS(fITSgeom));
  fMonitors.Add(new AliMonitorV0s);

  for (Int_t iMonitor = 0; iMonitor < fMonitors.GetEntriesFast(); iMonitor++) {
    ((AliMonitor*) fMonitors[iMonitor])->CreateHistos(fTopFolder);
  }

  TIterator* iFolder = fTopFolder->GetListOfFolders()->MakeIterator();
  while (TFolder* folder = (TFolder*) iFolder->Next()) folder->SetOwner(kTRUE);
  delete iFolder;

  fFile = TFile::Open("monitor_tree.root", "RECREATE");
  if (!fFile || !fFile->IsOpen()) {
    Fatal("AliMonitorProcess", "could not open file for tree");
  }
  fTree = new TTree("MonitorTree", "tree for monitoring");
  for (Int_t iMonitor = 0; iMonitor < fMonitors.GetEntriesFast(); iMonitor++) {
    ((AliMonitor*) fMonitors[iMonitor])->CreateBranches(fTree);
  }
  gROOT->cd();

  fServerSocket = new TServerSocket(kgPort, kTRUE);
  fServerSocket->SetOption(kNoBlock, 1);
  fDisplaySocket = NULL;
  CheckForConnections();

  fStatus = kStopped;
  fStopping = kFALSE;
}

//_____________________________________________________________________________
AliMonitorProcess::~AliMonitorProcess()
{
// clean up

  fMonitors.Delete();
  delete fTopFolder;
  delete fRunLoader;

  delete fServerSocket;
  fSockets.Delete();
  delete fDisplaySocket;

  fGrid->Close();
  delete fGrid;

  fFile->Close();
  delete fFile;
  gSystem->Unlink("monitor_tree.root");
}


//_____________________________________________________________________________
const char* AliMonitorProcess::GetRevision()
{
  return "$Revision$";
}


//_____________________________________________________________________________
void AliMonitorProcess::Run()
{
// run the monitor process: 
//  check for a raw data file, process the raw data file and delete it

  fStopping = kFALSE;

  while (!fStopping) {
    fStatus = kWaiting;
    while (!CheckForNewFile()) {
      CheckForConnections();
      fStatus = kWaiting;
      if (fStopping) break;
      gSystem->Sleep(10);
    }
    if (fStopping) break;

    ProcessFile();
  }

  WriteHistos();

  fStopping = kFALSE;
  fStatus = kStopped;
}


//_____________________________________________________________________________
void AliMonitorProcess::Stop()
{
// set the fStopping flag to terminate the monitor process after the current
// event was processed

  if (fStatus != kStopped) fStopping = kTRUE;
}


//_____________________________________________________________________________
void AliMonitorProcess::ProcessFile(const char* fileName)
{
// create a file with monitor histograms for a single file

  if (fStatus != kStopped) {
    Error("ProcessFile", "ProcessFile can not be called"
	  " while the monitor process is running");
    return;
  }

  fFileName = fileName;
  Int_t nEventMin = fNEventsMin;
  fNEventsMin = 1;
  ProcessFile();
  WriteHistos();
  fNEventsMin = nEventMin;
  fStatus = kStopped;
}


//_____________________________________________________________________________
Bool_t AliMonitorProcess::CheckForNewFile()
{
// check whether a new file was registered in alien

  TGridResult* result = fGrid->Ls();
  Long_t maxDate = -1;
  Long_t maxTime = -1;
  TString fileName;

  while (const char* entry = result->Next()) {
    // entry = host_date_time.root
    TString entryCopy(entry);
    char* p = const_cast<char*>(entryCopy.Data());
    if (!strsep(&p, "_") || !p) continue;  // host name
    char* dateStr = strsep(&p, "_");
    if (!dateStr || !p) continue;
    char* timeStr = strsep(&p, ".");
    if (!timeStr || !p) continue;
    Long_t date = atoi(dateStr);
    Long_t time = atoi(timeStr);

    if ((date > maxDate) || ((date == maxDate) && (time > maxTime))) {
      maxDate = date;
      maxTime = time;
      fileName = entry;
    }
  }

  if (maxDate < 0) return kFALSE;  // no files found
  if (fLogicalFileName.CompareTo(fileName) == 0) return kFALSE;  // no new file

  fLogicalFileName = fileName;
  fFileName = fGrid->GetPhysicalFileName(fLogicalFileName.Data());
  return kTRUE;
}

//_____________________________________________________________________________
Bool_t AliMonitorProcess::ProcessFile()
{
// loop over all events in the raw data file, run the reconstruction
// and fill the monitor histograms

  Int_t nEvents = GetNumberOfEvents(fFileName);
  if (nEvents <= 0) return kFALSE;
  Info("ProcessFile", "found %d event(s) in file %s", 
       nEvents, fFileName.Data());

  // loop over the events
  for (Int_t iEvent = 0; iEvent < nEvents; iEvent++) {
    fStatus = kReading;
    fRunLoader->SetEventNumber(0);
    AliRawReaderRoot rawReader(fFileName, iEvent);
    if (fStopping) break;
    if (rawReader.GetRunNumber() != fRunNumber) {
      WriteHistos();
      StartNewRun();
      fRunNumber = rawReader.GetRunNumber();
      fEventNumber[0] = rawReader.GetEventId()[0];
      fEventNumber[1] = rawReader.GetEventId()[1];
      fSubRunNumber = 0;
      if (fStopping) break;
    }

    if (!ReconstructTPC(&rawReader)) return kFALSE;
    if (fStopping) break;
    if (!ReconstructITS(&rawReader)) return kFALSE;
    if (fStopping) break;
    if (!ReconstructV0s()) return kFALSE;
    if (fStopping) break;

    if (fDisplaySocket) fDisplaySocket->Send("new event");

    Info("ProcessFile", "filling histograms...");
    fStatus = kFilling;
    for (Int_t iMonitor = 0; iMonitor < fMonitors.GetEntriesFast(); iMonitor++) {
      ((AliMonitor*) fMonitors[iMonitor])->FillHistos(fRunLoader, &rawReader);
      if (fStopping) break;
    }
    if (fStopping) break;

    Info("ProcessFile", "updating histograms...");
    fStatus = kUpdating;
    TIterator* iFolder = fTopFolder->GetListOfFolders()->MakeIterator();
    while (TFolder* folder = (TFolder*) iFolder->Next()) {
      TIterator* iHisto = folder->GetListOfFolders()->MakeIterator();
      while (AliMonitorPlot* histo = (AliMonitorPlot*) iHisto->Next()) {
	histo->Update();
      }
      delete iHisto;
    }
    delete iFolder;
    if (fStopping) break;

    Info("ProcessFile", "filling the tree...");
    fTree->Fill();

    Info("ProcessFile", "broadcasting histograms...");
    CheckForConnections();
    BroadcastHistos();

    fNEvents++;
    if (fStopping) break;
  }

  return kTRUE;
}

//_____________________________________________________________________________
void AliMonitorProcess::Reset()
{
// write the current histograms to a file and reset them

  if (fSubRunNumber == 0) fSubRunNumber++;
  WriteHistos();
  StartNewRun();
  fSubRunNumber++;
}


//_____________________________________________________________________________
UInt_t AliMonitorProcess::GetEventPeriodNumber()
{
// get the period number from the event id

  return (fEventNumber[1] >> 4);
}

//_____________________________________________________________________________
UInt_t AliMonitorProcess::GetEventOrbitNumber()
{
// get the orbit number from the event id

  return ((fEventNumber[1] & 0x000F) << 20) + (fEventNumber[0] >> 12);
}

//_____________________________________________________________________________
UInt_t AliMonitorProcess::GetEventBunchNumber()
{
// get the bunch number from the event id

  return (fEventNumber[0] % 0x0FFF);
}

//_____________________________________________________________________________
Int_t AliMonitorProcess::GetNumberOfEvents(const char* fileName)
{
// determine the number of events in the given raw data file

  Int_t nEvents = -1;

  TFile* file = TFile::Open(fileName);
  if (!file || !file->IsOpen()) {
    Error("GetNumberOfEvents", "could not open file %s", fileName);
    if (file) delete file;
    return -1;
  }

  TTree* tree = (TTree*) file->Get("RAW");
  if (!tree) {
    Error("GetNumberOfEvents", "could not find tree with raw data");
  } else {
    nEvents = (Int_t) tree->GetEntries();
  }
  file->Close();
  delete file;

  return nEvents;
}

//_____________________________________________________________________________
Bool_t AliMonitorProcess::ReconstructTPC(AliRawReader* rawReader)
{
// find TPC clusters and tracks

  fStatus = kRecTPC;

  AliLoader* tpcLoader = fRunLoader->GetLoader("TPCLoader");
  if (!tpcLoader) {
    Error("ReconstructTPC", "no TPC loader found");
    return kFALSE;
  }
  gSystem->Unlink("TPC.RecPoints.root");
  gSystem->Unlink("TPC.Tracks.root");

  // cluster finder
  Info("ReconstructTPC", "reconstructing clusters...");
  tpcLoader->LoadRecPoints("recreate");
  AliTPCclustererMI clusterer(fTPCParam);
  tpcLoader->MakeRecPointsContainer();
  clusterer.SetOutput(tpcLoader->TreeR());
  clusterer.Digits2Clusters(rawReader);
  tpcLoader->WriteRecPoints("OVERWRITE");

  // track finder
  Info("ReconstructTPC", "reconstructing tracks...");
  tpcLoader->LoadTracks("recreate");
  {
    AliTPCtrackerMI tracker(fTPCParam);
    tracker.Clusters2Tracks();
  }

  tpcLoader->UnloadRecPoints();
  tpcLoader->UnloadTracks();
  return kTRUE;
}

//_____________________________________________________________________________
Bool_t AliMonitorProcess::ReconstructITS(AliRawReader* rawReader)
{
// find ITS clusters and tracks

  fStatus = kRecITS;

  AliLoader* itsLoader = fRunLoader->GetLoader("ITSLoader");
  if (!itsLoader) {
    Error("ReconstructITS", "no ITS loader found");
    return kFALSE;
  }
  AliLoader* tpcLoader = fRunLoader->GetLoader("TPCLoader");
  if (!tpcLoader) {
    Error("ReconstructITS", "no TPC loader found");
    return kFALSE;
  }
  gSystem->Unlink("ITS.RecPoints.root");
  gSystem->Unlink("ITS.Tracks.root");

  // cluster finder
  Info("ReconstructITS", "reconstructing clusters...");
  itsLoader->LoadRecPoints("recreate");
  AliITSclustererV2 clusterer(fITSgeom);
  itsLoader->MakeRecPointsContainer();
  clusterer.Digits2Clusters(rawReader);

  // track finder
  Info("ReconstructITS", "reconstructing tracks...");
  itsLoader->LoadTracks("recreate");
  itsLoader->MakeTracksContainer();
  tpcLoader->LoadTracks();
  AliITStrackerV2 tracker(fITSgeom);
  tracker.LoadClusters(itsLoader->TreeR());
  tracker.Clusters2Tracks(tpcLoader->TreeT(), itsLoader->TreeT());
  tracker.UnloadClusters();
  itsLoader->WriteTracks("OVERWRITE");

  itsLoader->UnloadRecPoints();
  itsLoader->UnloadTracks();
  tpcLoader->UnloadTracks();
  return kTRUE;
}

//_____________________________________________________________________________
Bool_t AliMonitorProcess::ReconstructV0s()
{
// find V0s

  fStatus = kRecV0s;

  AliITSLoader* itsLoader = (AliITSLoader*) fRunLoader->GetLoader("ITSLoader");
  if (!itsLoader) {
    Error("ReconstructV0", "no ITS loader found");
    return kFALSE;
  }
  gSystem->Unlink("ITS.V0s.root");

  // V0 finder
  Info("ReconstructV0s", "reconstructing V0s...");
  itsLoader->LoadTracks("read");
  itsLoader->LoadV0s("recreate");
  AliV0vertexer vertexer;
  TTree* tracks = itsLoader->TreeT();
  if (!tracks) {
    Error("ReconstructV0s", "no ITS tracks tree found");
    return kFALSE;
  }
  if (!itsLoader->TreeV0()) itsLoader->MakeTree("V0");
  TTree* v0s = itsLoader->TreeV0();
  vertexer.Tracks2V0vertices(tracks, v0s);
  itsLoader->WriteV0s("OVERWRITE");

  itsLoader->UnloadTracks();
  itsLoader->UnloadV0s();
  return kTRUE;
}


//_____________________________________________________________________________
Bool_t AliMonitorProcess::WriteHistos()
{
// write the monitor tree and the monitor histograms to the file 
// "monitor_<run number>[_<sub_run_number>].root"
// if at least fNEventsMin events were monitored

  fStatus = kWriting;

  // rename tree file and create a new one
  fFile->cd();
  fTree->Write();
  fFile->Close();
  delete fFile;

  char fileName[256];
  sprintf(fileName, "monitor_tree_%d.root", fRunNumber);
  if (fSubRunNumber > 0) {
    sprintf(fileName, "monitor_tree_%d_%d.root", fRunNumber, fSubRunNumber);
  }
  if (fNEvents < fNEventsMin) {
    gSystem->Unlink("monitor_tree.root");
  } else {
    gSystem->Rename("monitor_tree.root", fileName);
  }

  fFile = TFile::Open("monitor_tree.root", "RECREATE");
  if (!fFile || !fFile->IsOpen()) {
    Fatal("WriteHistos", "could not open file for tree");
  }
  fTree = new TTree("MonitorTree", "tree for monitoring");
  for (Int_t iMonitor = 0; iMonitor < fMonitors.GetEntriesFast(); iMonitor++) {
    ((AliMonitor*) fMonitors[iMonitor])->CreateBranches(fTree);
  }
  gROOT->cd();

  // write the histograms
  if (fNEvents < fNEventsMin) return kTRUE;

  if (!fWriteHistoList) {
    TIterator* iFolder = fTopFolder->GetListOfFolders()->MakeIterator();
    while (TFolder* folder = (TFolder*) iFolder->Next()) {
      TIterator* iHisto = folder->GetListOfFolders()->MakeIterator();
      while (AliMonitorPlot* histo = (AliMonitorPlot*) iHisto->Next()) {
	histo->ResetList();
      }
      delete iHisto;
    }
    delete iFolder;
  }

  Bool_t result = kTRUE;
  sprintf(fileName, "monitor_%d.root", fRunNumber);
  if (fSubRunNumber > 0) {
    sprintf(fileName, "monitor_%d_%d.root", fRunNumber, fSubRunNumber);
  }
  TFile* file = TFile::Open(fileName, "recreate");
  if (!file || !file->IsOpen()) {
    Error("WriteHistos", "could not open file %s", fileName);
    result = kFALSE;
  } else {
    fTopFolder->Write();
    file->Close();
  }
  if (file) delete file;

  return result;
}

//_____________________________________________________________________________
void AliMonitorProcess::StartNewRun()
{
// reset the histograms for a new run

  fStatus = kResetting;
  TIterator* iFolder = fTopFolder->GetListOfFolders()->MakeIterator();
  while (TFolder* folder = (TFolder*) iFolder->Next()) {
    TIterator* iHisto = folder->GetListOfFolders()->MakeIterator();
    while (AliMonitorPlot* histo = (AliMonitorPlot*) iHisto->Next()) {
      histo->Reset();
    }
    delete iHisto;
  }
  delete iFolder;

  fNEvents = 0;
}


//_____________________________________________________________________________
void AliMonitorProcess::CheckForConnections()
{
// check if new clients want to connect and add them to the list of sockets

  TMessage message(kMESS_OBJECT);
  message.WriteObject(fTopFolder); 
  fStatus = kConnecting;

  TSocket* socket;
  while ((socket = fServerSocket->Accept()) != (TSocket*)-1) {
    char socketType[256];
    if (!socket->Recv(socketType, 255)) continue;
    if (strcmp(socketType, "client") == 0) {
      if ((fNEvents == 0) || (socket->Send(message) >= 0)) {
	fSockets.Add(socket);
	TInetAddress adr = socket->GetInetAddress();
	Info("CheckForConnections", "new client:\n %s (%s), port %d\n",
	     adr.GetHostName(), adr.GetHostAddress(), adr.GetPort());
      }
    } else if (strcmp(socketType, "display") == 0) {
      if (fDisplaySocket) {
	fDisplaySocket->Close();
	delete fDisplaySocket;
      }
      fDisplaySocket = socket;
      fDisplaySocket->SetOption(kNoBlock, 1);
      TInetAddress adr = socket->GetInetAddress();
      Info("CheckForConnections", "new display:\n %s (%s), port %d\n",
	   adr.GetHostName(), adr.GetHostAddress(), adr.GetPort());
    }
  }

  for (Int_t iSocket = 0; iSocket < fSockets.GetEntriesFast(); iSocket++) {
    socket = (TSocket*) fSockets[iSocket];
    if (!socket) continue;
    if (!socket->IsValid()) {
      // remove invalid sockets from the list
      TInetAddress adr = socket->GetInetAddress();
      Info("BroadcastHistos", "disconnect client:\n %s (%s), port %d\n",
	   adr.GetHostName(), adr.GetHostAddress(), adr.GetPort());
      delete fSockets.RemoveAt(iSocket);
    }
  }
  fSockets.Compress();
}

//_____________________________________________________________________________
void AliMonitorProcess::BroadcastHistos()
{
// send the monitor histograms to the clients

  fStatus = kBroadcasting;
  TMessage message(kMESS_OBJECT);
  message.WriteObject(fTopFolder); 

  for (Int_t iSocket = 0; iSocket < fSockets.GetEntriesFast(); iSocket++) {
    TSocket* socket = (TSocket*) fSockets[iSocket];
    if (!socket) continue;
    if (!socket->IsValid() || (socket->Send(message) < 0)) {
      // remove the socket from the list if there was an error
      TInetAddress adr = socket->GetInetAddress();
      Info("BroadcastHistos", "disconnect client:\n %s (%s), port %d\n",
	   adr.GetHostName(), adr.GetHostAddress(), adr.GetPort());
      delete fSockets.RemoveAt(iSocket);
    }
  }
  fSockets.Compress();
}
