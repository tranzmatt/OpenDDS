// -*- C++ -*-
//
// $Id$

#include  "EntryExit.h"
#include  "ace/ACE.h"

ACE_INLINE
TAO::DCPS::ThreadSynchResource::ThreadSynchResource(
  ACE_HANDLE handle, 
  ACE_Time_Value timeout)
  : handle_ (handle),
    timeout_ (timeout)
{
  DBG_ENTRY_LVL("ThreadSynchResource","ThreadSynchResource",5);
}

ACE_INLINE int
TAO::DCPS::ThreadSynchResource::wait_to_unclog()
{
  DBG_ENTRY_LVL("ThreadSynchResource","wait_to_unclog",5);

  if (ACE::handle_write_ready(this->handle_, &this->timeout_) == -1)
    {
      if (errno == ETIME)
        {
          ACE_ERROR((LM_ERROR, "(%P|%t) ERROR: handle_write_ready timed out\n"));
          this->notify_lost_on_backpressure_timeout ();
        }
      else
        {
          ACE_ERROR((LM_ERROR,
                    "(%P|%t) ERROR: ACE::handle_write_ready return -1 while waiting "
                    " to unclog. %p \n", "handle_write_ready"));
        }
      return -1;
    }
  return 0;
}


ACE_INLINE void 
TAO::DCPS::ThreadSynchResource::notify_lost_on_backpressure_timeout ()
{
  //noop
}

