/*
 *
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "DCPS/DdsDcps_pch.h" //Only the _pch include should start with DCPS/
#include "TransportInst.h"
#include "TransportImpl.h"
#include "TransportExceptions.h"
#include "EntryExit.h"
#include "DCPS/SafetyProfileStreams.h"

#include "ace/Configuration.h"

#include <cstring>
#include <algorithm>

#if !defined (__ACE_INLINE__)
# include "TransportInst.inl"
#endif /* !__ACE_INLINE__ */

OPENDDS_BEGIN_VERSIONED_NAMESPACE_DECL

OpenDDS::DCPS::TransportInst::~TransportInst()
{
  DBG_ENTRY_LVL("TransportInst","~TransportInst",6);
}

int
OpenDDS::DCPS::TransportInst::load(ACE_Configuration_Heap& cf,
                                   ACE_Configuration_Section_Key& sect)
{
  GET_CONFIG_VALUE(cf, sect, ACE_TEXT("queue_messages_per_pool"), this->queue_messages_per_pool_, size_t)
  GET_CONFIG_VALUE(cf, sect, ACE_TEXT("queue_initial_pools"), this->queue_initial_pools_, size_t)
  GET_CONFIG_VALUE(cf, sect, ACE_TEXT("max_packet_size"), this->max_packet_size_, ACE_UINT32)
  GET_CONFIG_VALUE(cf, sect, ACE_TEXT("max_samples_per_packet"), this->max_samples_per_packet_, size_t)
  GET_CONFIG_VALUE(cf, sect, ACE_TEXT("optimum_packet_size"), this->optimum_packet_size_, ACE_UINT32)
  GET_CONFIG_VALUE(cf, sect, ACE_TEXT("thread_per_connection"), this->thread_per_connection_, bool)
  GET_CONFIG_VALUE(cf, sect, ACE_TEXT("datalink_release_delay"), this->datalink_release_delay_, int)

  // Undocumented - this option is not in the Developer's Guide
  // Controls the number of chunks in the allocators used by the datalink
  // for control messages.
  GET_CONFIG_VALUE(cf, sect, ACE_TEXT("datalink_control_chunks"), this->datalink_control_chunks_, size_t)

  ACE_TString stringvalue;
  if (cf.get_string_value (sect, ACE_TEXT("passive_connect_duration"), stringvalue) == 0) {
    ACE_DEBUG ((LM_WARNING,
                ACE_TEXT ("(%P|%t) WARNING: passive_connect_duration option ")
                ACE_TEXT ("is deprecated in the transport inst, must be ")
                ACE_TEXT ("defined in transport config.\n")));
  }

  adjust_config_value();
  return 0;
}

void
OpenDDS::DCPS::TransportInst::dump() const
{
  ACE_DEBUG((LM_DEBUG,
             ACE_TEXT("\n(%P|%t) TransportInst::dump() -\n%C"),
             dump_to_str().c_str()));
}

namespace {
  static const int NAME_INDENT(3);
  static const int NAME_WIDTH(30); // Includes ":"
}

OPENDDS_STRING
OpenDDS::DCPS::TransportInst::formatNameForDump(const char* name)
{
  OPENDDS_STRING formatted_name;
  formatted_name.reserve(NAME_INDENT + NAME_WIDTH);
  formatted_name += OPENDDS_STRING(NAME_INDENT, ' ');
  formatted_name += name;
  formatted_name += ":";
  if ((NAME_WIDTH + NAME_INDENT) > formatted_name.length()) {
    formatted_name += OPENDDS_STRING((NAME_WIDTH + NAME_INDENT- formatted_name.length()), ' ');
  }
  return formatted_name;
}

OPENDDS_STRING
OpenDDS::DCPS::TransportInst::dump_to_str() const
{
  OPENDDS_STRING ret;
  ret += formatNameForDump("transport_type")          + this->transport_type_ + '\n';
  ret += formatNameForDump("name")                    + this->name_ + '\n';
  ret += formatNameForDump("queue_messages_per_pool") + to_dds_string(unsigned(this->queue_messages_per_pool_)) + '\n';
  ret += formatNameForDump("queue_initial_pools")     + to_dds_string(unsigned(this->queue_initial_pools_)) + '\n';
  ret += formatNameForDump("max_packet_size")         + to_dds_string(unsigned(this->max_packet_size_)) + '\n';
  ret += formatNameForDump("max_samples_per_packet")  + to_dds_string(unsigned(this->max_samples_per_packet_)) + '\n';
  ret += formatNameForDump("optimum_packet_size")     + to_dds_string(unsigned(this->optimum_packet_size_)) + '\n';
  ret += formatNameForDump("thread_per_connection")   + (this->thread_per_connection_ ? "true" : "false") + '\n';
  ret += formatNameForDump("datalink_release_delay")  + to_dds_string(this->datalink_release_delay_) + '\n';
  ret += formatNameForDump("datalink_control_chunks") + to_dds_string(unsigned(this->datalink_control_chunks_)) + '\n';
  return ret;
}

void
OpenDDS::DCPS::TransportInst::shutdown()
{
  ACE_GUARD(ACE_SYNCH_MUTEX, g, this->lock_);
  if (!this->impl_.is_nil()) {
    this->impl_->shutdown();
  }
}

OpenDDS::DCPS::TransportImpl_rch
OpenDDS::DCPS::TransportInst::impl()
{
  ACE_GUARD_RETURN(ACE_SYNCH_MUTEX, g, this->lock_, TransportImpl_rch());
  if (!this->impl_) {
    try {
      this->impl_ = this->new_impl();
    } catch (const OpenDDS::DCPS::Transport::UnableToCreate& ) {
      return TransportImpl_rch();
    }
  }
  return this->impl_;
}

void
OpenDDS::DCPS::TransportInst::set_port_in_addr_string(OPENDDS_STRING& addr_str, u_short port_number)
{
#ifdef BUFSIZE
#undef BUFSIZE
#endif
  const int BUFSIZE=1024;
  char result[BUFSIZE];

#ifdef __SUNPRO_CC
  int count = 0;
  std::count(addr_str.begin(), addr_str.end(), ':', count);
  if (count < 2) {
#else
  if (std::count(addr_str.begin(), addr_str.end(), ':') < 2) {
#endif
    OPENDDS_STRING::size_type pos = addr_str.find_last_of(":");
    ACE_OS::snprintf(result, BUFSIZE, "%.*s:%hu", static_cast<int>(pos), addr_str.c_str(), port_number);
  }
  else {
    // this is the numeric form of ipv6 address because it has more than one ':'
    if (addr_str[0] != '[') {
      ACE_OS::snprintf(result, BUFSIZE, "[%s]:%hu", addr_str.c_str(), port_number);
    }
    else {
      OPENDDS_STRING::size_type pos = addr_str.find_last_of("]");
      ACE_OS::snprintf(result, BUFSIZE, "%.*s:%hu", static_cast<int>(pos+1), addr_str.c_str(), port_number);
    }
  }
  addr_str = result;
}

OpenDDS::ICE::Endpoint*
OpenDDS::DCPS::TransportInst::get_ice_endpoint()
{
  return impl()->get_ice_endpoint();
}

OPENDDS_END_VERSIONED_NAMESPACE_DECL
