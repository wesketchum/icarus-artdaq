#include "icartdaq/Generators/PhysCrateData.hh"
#include "artdaq/Application/GeneratorMacros.hh"

#include <cerrno>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "icartdaq-core/Trace/trace_defines.h"
#include "ica_base/PhysCrate.h"
#include "ica_base/A2795.h"
#include "CAENComm.h"

icarus::PhysCrateData::PhysCrateData(fhicl::ParameterSet const & ps)
  :
  PhysCrate_GeneratorBase(ps),
  veto_host(ps.get<std::string>("VetoHost")),
  veto_host_port(ps.get<int>("VetoPort")),
  veto_udp(veto_host.c_str(),veto_host_port)
{
  InitializeHardware();
  InitializeVeto();
}

void icarus::PhysCrateData::InitializeVeto(){
  veto_state     = true; //defaults to on

  TRACE(TR_LOG,"IP ADDRESS for veto is %s:%d\n",veto_host.c_str(),veto_host_port);
  
  _doVetoTest    = ps_.get<bool>("DoVetoTest",false);
  if(_doVetoTest){
    _vetoTestPeriod = ps_.get<unsigned int>("VetoTestPeriod",1e6);
    share::ThreadFunctor functor = std::bind(&PhysCrateData::VetoTest,this);
    auto worker_functor = share::WorkerThreadFunctorUPtr(new share::WorkerThreadFunctor(functor,"VetoTestWorkerThread"));
    auto veto_worker = share::WorkerThread::createWorkerThread(worker_functor);
    _vetoTestThread.swap(veto_worker);
  }
}
  
void icarus::PhysCrateData::VetoOn(){
  TRACE(TR_LOG,"PhysCrateData::VetoOn called.");

  int result = veto_udp.VetoOn();
  TRACE(TR_LOG,"PhysCrateData::VetoOn called. Result %d",result);
  if(result<0)
    TRACE(TR_LOG,"PhysCrateData::VetoOn Error: %s",std::strerror(errno));

  veto_state = true;
}

void icarus::PhysCrateData::VetoOff(){
  TRACE(TR_LOG,"PhysCrateData::VetoOff called.");

  int result = veto_udp.VetoOff();
  TRACE(TR_LOG,"PhysCrateData::VetoOff called. Result %d",result);
  if(result<0)
    TRACE(TR_LOG,"PhysCrateData::VetoOff Error: %s",std::strerror(errno));

  veto_state = false;
}

void icarus::PhysCrateData::InitializeHardware(){
  physCr.reset(new PhysCrate());
  physCr->initialize();
}

BoardConf icarus::PhysCrateData::GetBoardConf(){

  auto const& ps_board = ps_.get<fhicl::ParameterSet>("BoardConfig");

  BoardConf config;
  config.sampInterval = 1e-9;
  config.delayTime = 0.0;
  config.nbrSegments = 1;
  config.coupling = 3;
  config.bandwidth = 0;
  config.fullScale = ps_board.get<double>("fullScale")*0.001;
  config.thresh = ps_board.get<int>("thresh");
  config.offset = ps_board.get<int>("offset") * config.fullScale/256;
  config.offsetadc = ps_board.get<int>("offset");

  return config;
}

TrigConf icarus::PhysCrateData::GetTrigConf(){

  auto const& ps_trig = ps_.get<fhicl::ParameterSet>("TriggerConfig");

  TrigConf config;
  config.trigClass = 0; // 0: Edge trigger
  config.sourcePattern = 0x00000002; // 0x00000001: channel 1, 0x00000002: channel 2
  config.trigCoupling = 0;            
  config.channel = 2;
  config.trigSlope = 0; // 0: positive, 1: negative
  config.trigLevel1 = -20.0; // In % of vertical full scale or mV if using an external trigger source.
  config.trigLevel2 = 0.0;
  config.nsamples = ps_trig.get<int>("mode")*1000 ;
  config.presamples = ps_trig.get<int>("trigmode")*1000 ;

  return config;
}

void icarus::PhysCrateData::ConfigureStart(){

  _tloop_start = std::chrono::high_resolution_clock::now();
  _tloop_end = std::chrono::high_resolution_clock::now();

  //physCr->configureTrig(GetTrigConf());
  //physCr->configure(GetBoardConf());
  VetoOff();
  physCr->start();

  if(_doVetoTest)
    _vetoTestThread->start();
}

void icarus::PhysCrateData::ConfigureStop(){
  if(_doVetoTest)
    _vetoTestThread->stop();
}

bool icarus::PhysCrateData::Monitor(){ 
  //usleep(1e5);
  /*  
  if(veto_state)
    usleep(1.5e6);
  */

  if(!veto_state){
    bool need_to_veto = false;
    
    for(int ib=0; ib<physCr->NBoards(); ++ib){
      auto status = physCr->BoardStatus(ib);
      
      std::string varname = ".Board_"+std::to_string(ib)+"_Status.last";
      metricMan_->sendMetric(varname,status,"Status",5,true,false);
      
      if( (status & STATUS_BUSY)!=0){
	TRACE(TR_ERROR,"PhysCrateData::Monitor : STATUS_BUSY on board %d!",ib);
	need_to_veto = true;
	break;
      }
    }
    
    if(need_to_veto && !veto_state)
      VetoOn();
    //else if(veto_state && !need_to_veto)
    //VetoOff();
  }
  
  if(veto_state)
    metricMan_->sendMetric(".VetoState.last",1,"state",5,true,false);
  else
    metricMan_->sendMetric(".VetoState.last",0,"state",5,true,false);    
  
  return true; 
}

bool icarus::PhysCrateData::VetoTest(){
  
  if(veto_state)
    VetoOff();
  else
    VetoOn();
  usleep(_vetoTestPeriod);
  
  return true;
}

int icarus::PhysCrateData::GetData(size_t & data_size, uint32_t* data_loc){

  TRACE(TR_LOG,"PhysCrateData::GetData called.");

  physCr->ArmTrigger();

  data_size=0;

  //end loop timer
  _tloop_end = std::chrono::high_resolution_clock::now();
  UpdateDuration();
  TRACE(TR_TIMER,"PhysCrateData::GetData : waitData loop time was %lf seconds",_tloop_duration.count());
  metricMan_->sendMetric(".GetData.ReturnTime.last",_tloop_duration.count()*1000.,"ms",5,true,true);
  metricMan_->sendMetric(".GetData.ReturnTime.max",_tloop_duration.count()*1000.,"ms",5,true,true);

  TRACE(TR_DEBUG,"PhysCrateData::GetData : Calling waitData()");
  physCr->waitData();

  //start loop timer
  _tloop_start = std::chrono::high_resolution_clock::now();

  _tloop_duration = std::chrono::duration_cast< std::chrono::duration<double> >(_tloop_end-_tloop_start);
  TRACE(TR_TIMER,"PhysCrateData::GetData : waitData call time was %lf seconds",_tloop_duration.count());
  metricMan_->sendMetric(".GetData.WaitTime.last",_tloop_duration.count()*1000.,"ms",5,true,true);
  metricMan_->sendMetric(".GetData.WaitTime.max",_tloop_duration.count()*1000.,"ms",5,true,true);

      
  while(physCr->dataAvail()){
    TRACE(TR_DEBUG,"PhysCrateData::GetData : DataAvail!");
    auto data_ptr = physCr->getData();
    TRACE(TR_DEBUG,"PhysCrateData::GetData : Data acquired! Size is %u, with %lu already acquired.",
	  ntohl(data_ptr->Header.packSize),data_size);
    
    if(ntohl(data_ptr->Header.packSize)==32) continue;
    
    auto ev_ptr = reinterpret_cast<uint32_t*>(data_ptr->data);    
    TRACE(TR_DEBUG,"PhysCrateData::GetData : Data event number is %#8X",*ev_ptr);
    
    std::copy((char*)data_ptr,
	      (char*)data_ptr+ntohl(data_ptr->Header.packSize),
	      (char*)data_loc+data_size);
    data_size += ntohl(data_ptr->Header.packSize);
    TRACE(TR_DEBUG,"PhysCrateData::GetData : Data copied! Size was %u, with %lu now acquired.",
	  ntohl(data_ptr->Header.packSize),data_size);
  }
  
  TRACE(TR_LOG,"PhysCrateData::GetData completed. Status %d, Data size %lu",0,data_size);

  if(data_size==0 && veto_state)
    VetoOff();

  return 0;
}

void icarus::PhysCrateData::FillStatPack( statpack & pack )
{

  memcpy(&(pack.token), "STAT", 4);
  //pack->crateid = htonl(158);
  pack.crateid = htonl(physCr->getId());
  pack.memstat1 = 0;
  pack.memstat2 = 0;
  pack.size = htonl(28);

  std::cout << "statpack initilized..." << std::endl;

  //return pack;
}

DEFINE_ARTDAQ_COMMANDABLE_GENERATOR(icarus::PhysCrateData) 
