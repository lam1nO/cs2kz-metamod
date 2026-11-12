#include <limits>
#include <string>
#define max(a,b) forbidden_windows_max
#include "../src/kz/timer/cyb_ticket_lease.h"
#include <cassert>
#undef max
int main(){
 const std::string a="01a1d050-54b5-7950-959d-035b7be030ab",b="01a1d050-54b5-7950-959d-035b7be030ac";
 CybTickets::Lease lease;assert(lease.Capture(1000).empty());
 assert(lease.Update(a,90000,1000,-1));auto queued=lease.Capture(1001);assert(queued==a);
 assert(!lease.Update(b,90000,1002,0));assert(lease.Capture(1002)==a);
 assert(lease.Capture(90999)==a);assert(lease.Capture(91000).empty());
 assert(lease.Update(b,5000,92000,-1));assert(lease.Capture(92001)==b);assert(queued==a);
 assert(lease.Update("clear",0,92002,-1));assert(lease.Capture(92002).empty());
 assert(lease.Update(a,5000,93000,-1));assert(!lease.Update(b,90001,93001,-1));assert(lease.Capture(93001).empty());
 assert(!lease.Update("bad;command",1000,94000,-1));assert(lease.Capture(94001).empty());
}
