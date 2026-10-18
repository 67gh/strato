#include "interval.h"
#include "queue.h"
#include <thread>
#include <cassert>
#include <random>
#include <iostream>
using skyline::IntervalList;
int main(){
 IntervalList<size_t> a;a.Insert({4,8});assert(!a.Query(8).enclosed);assert(!a.Intersect({8,12}));assert(a.Query(4).size==4);
 a.Insert({12,12});a.Insert({20,18});assert(!a.Query(12).enclosed);
 IntervalList<size_t> b;b.Merge(a,16);assert(b.Query(20).enclosed);assert(!b.Query(24).enclosed);b.Merge(b);b.Merge(b,8);assert(b.Query(28).enclosed);
 std::mt19937 gen(42);IntervalList<size_t> all;std::vector<bool> model(256);
 for(int n=0;n<3000;n++){size_t start=gen()%256,end=gen()%256;all.Insert({start,end});if(start<end)for(auto i=start;i<end;i++)model[i]=true;for(size_t i=0;i<256;i++)assert(all.Query(i).enclosed==model[i]);}
 skyline::CircularQueue<int> empty(2);std::thread consumer([&]{empty.Process([](int){assert(false);},[]{});});empty.Close();consumer.join();
 skyline::CircularQueue<int> drain(4);drain.Push(3);drain.Push(7);drain.Close();int sum=0;drain.Process([&](int n){sum+=n;},[]{});assert(sum==10);
 bool threw=false;try{drain.Push(1);}catch(const std::runtime_error&){threw=true;}assert(threw);
 skyline::CircularQueue<int> q(16);std::atomic<int> received{};std::vector<std::thread> readers;
 for(int n=0;n<3;n++)readers.emplace_back([&]{for(;;){try{received+=q.Pop();}catch(const std::runtime_error&){break;}}});
 std::vector<std::thread> writers;for(int n=0;n<3;n++)writers.emplace_back([&]{for(int i=0;i<4000;i++)q.Push(1);});for(auto&t:writers)t.join();q.Close();for(auto&t:readers)t.join();assert(received==12000);
 for(int i=0;i<100;i++) {
  skyline::CircularQueue<int> full(1);full.Push(1);std::atomic<bool> entered{},rejected{};
  std::thread blocked([&]{entered=true;try{full.Push(2);}catch(const std::runtime_error&){rejected=true;}});
  while(!entered)std::this_thread::yield();full.Close();blocked.join();assert(rejected);assert(full.Pop()==1);
 }
 skyline::CircularQueue<int> batch(8);std::atomic<int> batchSum{};
 std::thread batchReader([&]{batch.Process([&](int n){batchSum+=n;},[]{});});
 std::vector<std::thread> batchWriters;for(int n=0;n<4;n++)batchWriters.emplace_back([&]{for(int i=0;i<1000;i++)batch.Push(1);});
 for(auto&t:batchWriters)t.join();batch.Close();batchReader.join();assert(batchSum==4000);
 std::cout<<"PASS half-open intervals, empty/reversed input, translated/self merge, 768000 model comparisons; queue empty-close, drain, reject after close, 3 producers/3 consumers; full-close blocked producers x100; Process with four producers\n";
}
