//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2016 Jeongnim Kim and QMCPACK developers.
//
// File developed by:  Mark Dewing, markdewing@gmail.com, Argonne National Laboratory
//
// File created by: Mark Dewing, markdewing@gmail.com, Argonne National Laboratory
//////////////////////////////////////////////////////////////////////////////////////


#include "catch.hpp"

#define USE_FAKE_CLOCK
#include "Utilities/NewTimer.h"
#include <stdio.h>
#include <string>
#include <vector>

namespace qmcplusplus {


// Used by fake_cpu_clock in Clock.h if USE_FAKE_CLOCK is defined
double fake_cpu_clock_increment = 1.0;
double fake_cpu_clock_value = 0.0;

class FakeTimer : public NewTimer
{
public:
  FakeTimer(const std::string& myname) : NewTimer(myname) {}

  void set_total_time(double my_total_time)
  {
    total_time = my_total_time;
  }

  void set_num_calls(long my_num_calls)
  {
    num_calls = my_num_calls;
  }

};

TEST_CASE("test_timer_stack", "[utilities]")
{
  // Use a local version rather than the global TimerManager, otherwise
  //  changes will persist from test to test.
  TimerManagerClass tm;
  NewTimer t1("timer1");
  tm.addTimer(&t1);
#if ENABLE_TIMER
#ifdef USE_STACK_TIMERS
  t1.start();
  REQUIRE(tm.current_timer() == &t1);
  t1.stop();
  REQUIRE(tm.current_timer() == NULL);
#endif
#endif
}

TEST_CASE("test_timer_flat_profile", "[utilities]")
{
  TimerManagerClass tm;
  FakeTimer t1("timer1");
  tm.addTimer(&t1);
  t1.set_total_time(1.1);
  t1.set_num_calls(2);

  TimerManagerClass::nameList_t nameList;
  TimerManagerClass::timeList_t timeList;
  TimerManagerClass::callList_t callList;
  tm.collate_flat_profile(NULL, nameList, timeList, callList);

  REQUIRE(nameList.size() == 1);
  REQUIRE(nameList.at("timer1") == 0);
  REQUIRE(timeList.size() == 1);
  REQUIRE(timeList[0] == Approx(1.1));
  REQUIRE(callList.size() == 1);
  REQUIRE(callList[0] == 2);
}

TEST_CASE("test_timer_flat_profile_same_name", "[utilities]")
{
  TimerManagerClass tm;
  FakeTimer t1("timer1");
  tm.addTimer(&t1);
  FakeTimer t2("timer2");
  tm.addTimer(&t2);
  FakeTimer t3("timer1");
  tm.addTimer(&t3);

  fake_cpu_clock_increment = 1.1;
  t1.start();
  t1.stop();
  fake_cpu_clock_increment = 1.2;
  for (int i = 0; i < 3; i++)
  {
    t2.start();
    t2.stop();

    t3.start();
    t3.stop();
  }
  t3.start();
  t3.stop();

  TimerManagerClass::nameList_t nameList;
  TimerManagerClass::timeList_t timeList;
  TimerManagerClass::callList_t callList;
  tm.collate_flat_profile(NULL, nameList, timeList, callList);

  REQUIRE(nameList.size() == 2);
  int idx1 = nameList.at("timer1");
  int idx2 = nameList.at("timer2");
  REQUIRE(timeList.size() == 2);
  REQUIRE(timeList[idx1] == Approx(5.9));
  REQUIRE(timeList[idx2] == Approx(3.6));

  REQUIRE(callList.size() == 2);
  REQUIRE(callList[idx1] == 5);
  REQUIRE(callList[idx2] == 3);
}

TEST_CASE("test_timer_nested_profile", "[utilities]")
{
  TimerManagerClass tm;
  FakeTimer t1("timer1");
  tm.addTimer(&t1);
  FakeTimer t2("timer2");
  tm.addTimer(&t2);

  fake_cpu_clock_increment = 1.1;
  t1.start();
  t2.start();
  t2.stop();
  t1.stop();

  TimerManagerClass::nameList_t nameList;
  TimerManagerClass::timeList_t timeList;
  TimerManagerClass::callList_t callList;
  tm.collate_flat_profile(NULL, nameList, timeList, callList);

  REQUIRE(nameList.size() == 2);
  int idx1 = nameList.at("timer1");
  int idx2 = nameList.at("timer2");
  REQUIRE(timeList.size() == 2);
  REQUIRE(timeList[idx1] == Approx(3*fake_cpu_clock_increment));
  REQUIRE(timeList[idx2] == Approx(fake_cpu_clock_increment));

  TimerManagerClass::nameList_t nameList2;
  TimerManagerClass::timeList_t timeList2;
  TimerManagerClass::timeList_t timeExclList2;
  TimerManagerClass::callList_t callList2;
  tm.collate_stack_profile(NULL, nameList2, timeList2, timeExclList2, callList2);

  REQUIRE(nameList2.size() == 2);
  idx1 = nameList2.at("timer1");
  idx2 = nameList2.at("timer2/timer1");
  REQUIRE(timeList2.size() == 2);
  REQUIRE(timeExclList2.size() == 2);
  REQUIRE(timeList2[idx1] == Approx(3*fake_cpu_clock_increment));
  REQUIRE(timeList2[idx2] == Approx(fake_cpu_clock_increment));

  // Time in t1 minus time inside t2
  REQUIRE(timeExclList2[idx1] == Approx(2*fake_cpu_clock_increment));
  REQUIRE(timeExclList2[idx2] == Approx(fake_cpu_clock_increment));
}

}
