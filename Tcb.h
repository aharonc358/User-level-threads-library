
#ifndef _TCB_H_
#define _TCB_H_

#include <setjmp.h>
#include <sys/time.h>
#include <bits/sigaction.h>
#include <queue>
#include <vector>
#include <new>
#include "uthreads.h"
#include <iostream>
#include <list>


typedef void (*thread_entry_point)(void);

typedef bool(*Comparator)(const std::pair<int, int>&, const std::pair<int, int>&);

typedef std::priority_queue<std::pair<int, int>, std::vector<std::pair<int,
    int>>, bool(*)(const std::pair<int, int>&, const std::pair<int, int>&)>
min_heap_pairs;







class Tcb
{
 public:
  sigjmp_buf env ;
  int id;
  char stack[STACK_SIZE];
  int num_of_quantums;
  sigset_t old_mask;
  bool is_sleeping;
  bool is_blocked;

  Tcb(int  id){
    this->id = id;
    num_of_quantums = 0;
    is_blocked = false;
    is_sleeping = false;
  }

};


bool comparator(const std::pair<int, int>& lhs, const std::pair<int, int>& rhs) {
  // Compare based on the first element of the pairs
  return lhs.first > rhs.first;
}



class MyQueue {
 private:
  std::list<Tcb*> container;

 public:
  // Function to push an element into the queue
  void push(Tcb* value) {
    container.push_back(value);
  }

  // Function to remove a specific element by value
  void remove( Tcb* value) {
    container.remove(value);
  }

  // Function to remove the front element of the queue
  void pop() {
    if (!empty()) {
      container.pop_front();
    }
  }

  // Function to get the front element of the queue
  Tcb* front() {
    return container.front();
  }

  // Function to check if the queue is empty
  bool empty() const {
    return container.empty();
  }

  // Function to get the size of the queue
  size_t size() const {
    return container.size();
  }
};


#endif //_TCB_H_
