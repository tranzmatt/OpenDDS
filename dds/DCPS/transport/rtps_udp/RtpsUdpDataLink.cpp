/*
 *
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "RtpsUdpDataLink.h"
#include "RtpsUdpTransport.h"
#include "RtpsUdpInst.h"
#include "RtpsUdpSendStrategy.h"
#include "RtpsUdpReceiveStrategy.h"

#include "dds/DCPS/transport/framework/TransportCustomizedElement.h"
#include "dds/DCPS/transport/framework/TransportSendElement.h"
#include "dds/DCPS/transport/framework/TransportSendControlElement.h"
#include "dds/DCPS/transport/framework/NetworkAddress.h"

#include "dds/DCPS/RTPS/RtpsCoreTypeSupportImpl.h"
#include "dds/DCPS/RTPS/BaseMessageUtils.h"
#include "dds/DCPS/RTPS/BaseMessageTypes.h"
#include "dds/DCPS/RTPS/MessageTypes.h"

#ifdef OPENDDS_SECURITY
#include "dds/DCPS/RTPS/SecurityHelpers.h"
#include "dds/DCPS/security/framework/SecurityRegistry.h"
#endif

#include "dds/DdsDcpsCoreTypeSupportImpl.h"

#include "dds/DCPS/Definitions.h"

#include "ace/Default_Constants.h"
#include "ace/Log_Msg.h"
#include "ace/Message_Block.h"
#include "ace/Reactor.h"

#include <string.h>

#ifndef __ACE_INLINE__
# include "RtpsUdpDataLink.inl"
#endif  /* __ACE_INLINE__ */

namespace {

/// Return the number of CORBA::Longs required for the bitmap representation of
/// sequence numbers between low and high, inclusive (maximum 8 longs).
CORBA::ULong
bitmap_num_longs(const OpenDDS::DCPS::SequenceNumber& low,
                 const OpenDDS::DCPS::SequenceNumber& high)
{
  return high < low ? CORBA::ULong(1) : std::min(CORBA::ULong(8), CORBA::ULong((high.getValue() - low.getValue() + 32) / 32));
}

bool bitmapNonEmpty(const OpenDDS::RTPS::SequenceNumberSet& snSet)
{
  for (CORBA::ULong i = 0; i < snSet.bitmap.length(); ++i) {
    if (snSet.bitmap[i]) {
      if (snSet.numBits >= (i + 1) * 32) {
        return true;
      }
      for (int bit = 31; bit >= 0; --bit) {
        if ((snSet.bitmap[i] & (1 << bit))
            && snSet.numBits >= i * 32 + (31 - bit)) {
          return true;
        }
      }
    }
  }
  return false;
}

}

OPENDDS_BEGIN_VERSIONED_NAMESPACE_DECL

namespace OpenDDS {
namespace DCPS {

RtpsUdpDataLink::RtpsUdpDataLink(RtpsUdpTransport& transport,
                                 const GuidPrefix_t& local_prefix,
                                 const RtpsUdpInst& config,
                                 const ReactorTask_rch& reactor_task)
  : DataLink(transport, // 3 data link "attributes", below, are unused
             0,         // priority
             false,     // is_loopback
             false)     // is_active
  , reactor_task_(reactor_task)
  , multi_buff_(this, config.nak_depth_)
  , best_effort_heartbeat_count_(0)
  , nack_reply_(this, &RtpsUdpDataLink::send_nack_replies,
                config.nak_response_delay_)
  , heartbeat_reply_(this, &RtpsUdpDataLink::send_heartbeat_replies,
                     config.heartbeat_response_delay_)
  , heartbeat_(make_rch<HeartBeat>(reactor_task->get_reactor(), reactor_task->get_reactor_owner(), this, &RtpsUdpDataLink::send_heartbeats))
  , heartbeatchecker_(make_rch<HeartBeat>(reactor_task->get_reactor(), reactor_task->get_reactor_owner(), this, &RtpsUdpDataLink::check_heartbeats))
  , relay_beacon_(make_rch<HeartBeat>(reactor_task->get_reactor(), reactor_task->get_reactor_owner(), this, &RtpsUdpDataLink::send_relay_beacon))
  , held_data_delivery_handler_(this)
  , max_bundle_size_(config.max_bundle_size_)
#ifdef OPENDDS_SECURITY
  , security_config_(Security::SecurityRegistry::instance()->default_config())
  , local_crypto_handle_(DDS::HANDLE_NIL)
#endif
{
  this->send_strategy_ = make_rch<RtpsUdpSendStrategy>(this, local_prefix);
  this->receive_strategy_ = make_rch<RtpsUdpReceiveStrategy>(this, local_prefix);
  std::memcpy(local_prefix_, local_prefix, sizeof(GuidPrefix_t));
}

RtpsUdpInst&
RtpsUdpDataLink::config() const
{
  return static_cast<RtpsUdpTransport&>(impl()).config();
}

bool
RtpsUdpDataLink::add_delayed_notification(TransportQueueElement* element)
{
  RtpsWriterMap::iterator iter = writers_.find(element->publication_id());
  if (iter != writers_.end()) {

    iter->second->add_elem_awaiting_ack(element);
    return true;
  }
  return false;
}

void RtpsUdpDataLink::do_remove_sample(const RepoId& pub_id,
  const TransportQueueElement::MatchCriteria& criteria)
{
  ACE_GUARD(ACE_Thread_Mutex, g, lock_);
  RtpsWriter_rch writer;
  RtpsWriterMap::iterator iter = writers_.find(pub_id);
  if (iter != writers_.end()) {
    writer = iter->second;
  }

  g.release();

  if (writer) {
    writer->do_remove_sample(criteria);
  }
}

void
RtpsUdpDataLink::RtpsWriter::do_remove_sample(const TransportQueueElement::MatchCriteria& criteria)
{
  SnToTqeMap sn_tqe_map;
  SnToTqeMap to_deliver;

  ACE_GUARD(ACE_Thread_Mutex, g, mutex_);

  RtpsUdpDataLink_rch link = link_.lock();
  if (!link) {
    return;
  }

  if (!elems_not_acked_.empty()) {
    to_deliver.insert(to_deliver_.begin(), to_deliver_.end());
    to_deliver_.clear();
    OPENDDS_SET(SequenceNumber) sns_to_release;
    SnToTqeMap::iterator it = elems_not_acked_.begin();
    while (it != elems_not_acked_.end()) {
      if (criteria.matches(*it->second)) {
        sn_tqe_map.insert(RtpsWriter::SnToTqeMap::value_type(it->first, it->second));
        sns_to_release.insert(it->first);
        SnToTqeMap::iterator last = it;
        ++it;
        elems_not_acked_.erase(last);
      } else {
        ++it;
      }
    }
    OPENDDS_SET(SequenceNumber)::iterator sns_it = sns_to_release.begin();
    while (sns_it != sns_to_release.end()) {
      send_buff_->release_acked(*sns_it);
      ++sns_it;
    }
  }

  g.release();

  SnToTqeMap::iterator deliver_iter = to_deliver.begin();
  while (deliver_iter != to_deliver.end()) {
    deliver_iter->second->data_delivered();
    ++deliver_iter;
  }
  SnToTqeMap::iterator drop_iter = sn_tqe_map.begin();
  while (drop_iter != sn_tqe_map.end()) {
    drop_iter->second->data_dropped(true);
    ++drop_iter;
  }
}

bool
RtpsUdpDataLink::open(const ACE_SOCK_Dgram& unicast_socket)
{
  unicast_socket_ = unicast_socket;

  RtpsUdpInst& config = this->config();

  if (config.use_multicast_) {
    const OPENDDS_STRING& net_if = config.multicast_interface_;
#ifdef ACE_HAS_MAC_OSX
    multicast_socket_.opts(ACE_SOCK_Dgram_Mcast::OPT_BINDADDR_NO |
                           ACE_SOCK_Dgram_Mcast::DEFOPT_NULLIFACE);
#endif
    if (multicast_socket_.join(config.multicast_group_address_, 1,
                               net_if.empty() ? 0 :
                               ACE_TEXT_CHAR_TO_TCHAR(net_if.c_str())) != 0) {
      ACE_ERROR_RETURN((LM_ERROR,
                        ACE_TEXT("(%P|%t) ERROR: ")
                        ACE_TEXT("RtpsUdpDataLink::open: ")
                        ACE_TEXT("ACE_SOCK_Dgram_Mcast::join failed: %m\n")),
                       false);
    }
  }

  if (!OpenDDS::DCPS::set_socket_multicast_ttl(unicast_socket_, config.ttl_)) {
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("RtpsUdpDataLink::open: ")
                      ACE_TEXT("failed to set TTL: %d\n"),
                      config.ttl_),
                     false);
  }

  if (config.send_buffer_size_ > 0) {
    int snd_size = config.send_buffer_size_;
    if (this->unicast_socket_.set_option(SOL_SOCKET,
                                SO_SNDBUF,
                                (void *) &snd_size,
                                sizeof(snd_size)) < 0
        && errno != ENOTSUP) {
      ACE_ERROR_RETURN((LM_ERROR,
                        ACE_TEXT("(%P|%t) ERROR: ")
                        ACE_TEXT("RtpsUdpDataLink::open: failed to set the send buffer size to %d errno %m\n"),
                        snd_size),
                       false);
    }
  }

  if (config.rcv_buffer_size_ > 0) {
    int rcv_size = config.rcv_buffer_size_;
    if (this->unicast_socket_.set_option(SOL_SOCKET,
                                SO_RCVBUF,
                                (void *) &rcv_size,
                                sizeof(int)) < 0
        && errno != ENOTSUP) {
      ACE_ERROR_RETURN((LM_ERROR,
                        ACE_TEXT("(%P|%t) ERROR: ")
                        ACE_TEXT("RtpsUdpDataLink::open: failed to set the receive buffer size to %d errno %m \n"),
                        rcv_size),
                       false);
    }
  }

  send_strategy()->send_buffer(&multi_buff_);

  if (start(send_strategy_,
            receive_strategy_, false) != 0) {
    stop_i();
    ACE_ERROR_RETURN((LM_ERROR,
                      ACE_TEXT("(%P|%t) ERROR: ")
                      ACE_TEXT("UdpDataLink::open: start failed!\n")),
                     false);
  }

  return true;
}


void
RtpsUdpDataLink::add_locator(const RepoId& remote_id,
                             const ACE_INET_Addr& address,
                             bool requires_inline_qos)
{
  ACE_GUARD(ACE_Thread_Mutex, g, lock_);
  locators_[remote_id] = RemoteInfo(address, requires_inline_qos);
}

void
RtpsUdpDataLink::associated(const RepoId& local_id, const RepoId& remote_id,
                            bool local_reliable, bool remote_reliable,
                            bool local_durable, bool remote_durable)
{
  const GuidConverter conv(local_id);

  if (conv.isReader() && config().rtps_relay_address() != ACE_INET_Addr()) {
    relay_beacon_->schedule_enable(false);
  }

  if (!local_reliable) {
    return;
  }

  bool enable_heartbeat = false;

  ACE_GUARD(ACE_Thread_Mutex, g, lock_);
  if (conv.isWriter()) {
    if (remote_reliable) {
      // Insert count if not already there.
      RtpsWriterMap::iterator rw = writers_.find(local_id);
      if (rw == writers_.end()) {
        RtpsUdpDataLink_rch link(this, OpenDDS::DCPS::inc_count());
        CORBA::Long hb_start = 0;
        HeartBeatCountMapType::iterator hbc_it = heartbeat_counts_.find(local_id);
        if (hbc_it != heartbeat_counts_.end()) {
          hb_start = hbc_it->second;
          heartbeat_counts_.erase(hbc_it);
        }
        RtpsWriter_rch writer = make_rch<RtpsWriter>(link, local_id, local_durable, hb_start);
        rw = writers_.insert(RtpsWriterMap::value_type(local_id, writer)).first;
      }
      RtpsWriter_rch writer = rw->second;
      enable_heartbeat = true;
      g.release();
      writer->add_reader(remote_id, ReaderInfo(remote_durable));
    } else {
      invoke_on_start_callbacks(local_id, remote_id, true);
    }
  } else if (conv.isReader()) {
    RtpsReaderMap::iterator rr = readers_.find(local_id);
    if (rr == readers_.end()) {
      RtpsUdpDataLink_rch link(this, OpenDDS::DCPS::inc_count());
      RtpsReader_rch reader = make_rch<RtpsReader>(link, local_id, local_durable);
      rr = readers_.insert(RtpsReaderMap::value_type(local_id, reader)).first;
    }
    RtpsReader_rch reader = rr->second;
    readers_of_writer_.insert(RtpsReaderMultiMap::value_type(remote_id, rr->second));
    g.release();
    reader->add_writer(remote_id, WriterInfo());
  }

  if (enable_heartbeat) {
    heartbeat_->schedule_enable(true);
  }
}

bool
RtpsUdpDataLink::check_handshake_complete(const RepoId& local_id,
                                          const RepoId& remote_id)
{
  const GuidConverter conv(local_id);
  if (conv.isWriter()) {
    RtpsWriterMap::iterator rw = writers_.find(local_id);
    if (rw == writers_.end()) {
      return true; // not reliable, no handshaking
    }
    return rw->second->is_reader_handshake_done(remote_id);
  } else if (conv.isReader()) {
    return true; // no handshaking for local reader
  }
  return false;
}

void
RtpsUdpDataLink::register_for_reader(const RepoId& writerid,
                                     const RepoId& readerid,
                                     const ACE_INET_Addr& address,
                                     OpenDDS::DCPS::DiscoveryListener* listener)
{
  ACE_GUARD(ACE_Thread_Mutex, g, lock_);
  bool enableheartbeat = interesting_readers_.empty();
  interesting_readers_.insert(InterestingRemoteMapType::value_type(readerid, InterestingRemote(writerid, address, listener)));
  if (heartbeat_counts_.find(writerid) == heartbeat_counts_.end()) {
    heartbeat_counts_[writerid] = 0;
  }
  g.release();
  if (enableheartbeat) {
    heartbeat_->schedule_enable(false);
  }
}

void
RtpsUdpDataLink::unregister_for_reader(const RepoId& writerid,
                                       const RepoId& readerid)
{
  ACE_GUARD(ACE_Thread_Mutex, g, lock_);
  for (InterestingRemoteMapType::iterator pos = interesting_readers_.lower_bound(readerid),
         limit = interesting_readers_.upper_bound(readerid);
       pos != limit;
       ) {
    if (pos->second.localid == writerid) {
      interesting_readers_.erase(pos++);
    } else {
      ++pos;
    }
  }
}

void
RtpsUdpDataLink::register_for_writer(const RepoId& readerid,
                                     const RepoId& writerid,
                                     const ACE_INET_Addr& address,
                                     OpenDDS::DCPS::DiscoveryListener* listener)
{
  ACE_GUARD(ACE_Thread_Mutex, g, lock_);
  bool enableheartbeatchecker = interesting_writers_.empty();
  interesting_writers_.insert(InterestingRemoteMapType::value_type(writerid, InterestingRemote(readerid, address, listener)));
  g.release();
  if (enableheartbeatchecker) {
    heartbeatchecker_->schedule_enable(false);
  }
}

void
RtpsUdpDataLink::unregister_for_writer(const RepoId& readerid,
                                       const RepoId& writerid)
{
  ACE_GUARD(ACE_Thread_Mutex, g, lock_);
  for (InterestingRemoteMapType::iterator pos = interesting_writers_.lower_bound(writerid),
         limit = interesting_writers_.upper_bound(writerid);
       pos != limit;
       ) {
    if (pos->second.localid == readerid) {
      interesting_writers_.erase(pos++);
    } else {
      ++pos;
    }
  }
}

void
RtpsUdpDataLink::RtpsWriter::pre_stop_helper(OPENDDS_VECTOR(TransportQueueElement*)& to_deliver,
                                             OPENDDS_VECTOR(TransportQueueElement*)& to_drop)
{
  typedef OPENDDS_MULTIMAP(SequenceNumber, TransportQueueElement*)::iterator iter_t;

  ACE_GUARD(ACE_Thread_Mutex, g, mutex_);
  if (!to_deliver_.empty()) {
    iter_t iter = to_deliver_.begin();
    while (iter != to_deliver_.end()) {
      to_deliver.push_back(iter->second);
      to_deliver_.erase(iter);
      iter = to_deliver_.begin();
    }
  }
  if (!elems_not_acked_.empty()) {
    OPENDDS_SET(SequenceNumber) sns_to_release;
    iter_t iter = elems_not_acked_.begin();
    while (iter != elems_not_acked_.end()) {
      to_drop.push_back(iter->second);
      sns_to_release.insert(iter->first);
      elems_not_acked_.erase(iter);
      iter = elems_not_acked_.begin();
    }
    OPENDDS_SET(SequenceNumber)::iterator sns_it = sns_to_release.begin();
    while (sns_it != sns_to_release.end()) {
      send_buff_->release_acked(*sns_it);
      ++sns_it;
    }
  }
}

void
RtpsUdpDataLink::pre_stop_i()
{
  DBG_ENTRY_LVL("RtpsUdpDataLink","pre_stop_i",6);
  DataLink::pre_stop_i();
  OPENDDS_VECTOR(TransportQueueElement*) to_deliver;
  OPENDDS_VECTOR(TransportQueueElement*) to_drop;
  {
    ACE_GUARD(ACE_Thread_Mutex, g, lock_);

    RtpsWriterMap::iterator iter = writers_.begin();
    while (iter != writers_.end()) {
      RtpsWriter_rch writer = iter->second;
      writer->pre_stop_helper(to_deliver, to_drop);
      RtpsWriterMap::iterator last = iter;
      ++iter;
      heartbeat_counts_.erase(last->first);
      writers_.erase(last);
    }
  }
  typedef OPENDDS_VECTOR(TransportQueueElement*)::iterator tqe_iter;
  tqe_iter deliver_it = to_deliver.begin();
  while (deliver_it != to_deliver.end()) {
    (*deliver_it)->data_delivered();
    ++deliver_it;
  }
  tqe_iter drop_it = to_drop.begin();
  while (drop_it != to_drop.end()) {
    (*drop_it)->data_dropped(true);
    ++drop_it;
  }
}

void
RtpsUdpDataLink::release_reservations_i(const RepoId& remote_id,
                                        const RepoId& local_id)
{
  OPENDDS_VECTOR(TransportQueueElement*) to_deliver;
  OPENDDS_VECTOR(TransportQueueElement*) to_drop;
  using std::pair;
  const GuidConverter conv(local_id);
  if (conv.isWriter()) {
    ACE_GUARD(ACE_Thread_Mutex, g, lock_);
    RtpsWriterMap::iterator rw = writers_.find(local_id);

    if (rw != writers_.end()) {
      RtpsWriter_rch writer = rw->second;
      g.release();
      writer->remove_reader(remote_id);

      if (writer->reader_count() == 0) {
        writer->pre_stop_helper(to_deliver, to_drop);
        const CORBA::ULong hbc = writer->get_heartbeat_count();

        ACE_GUARD(ACE_Thread_Mutex, h, lock_);
        rw = writers_.find(local_id);
        if (rw != writers_.end()) {
          heartbeat_counts_[rw->first] = hbc;
          writers_.erase(rw);
        }
      } else {
        writer->process_acked_by_all();
      }
    }

  } else if (conv.isReader()) {
    ACE_GUARD(ACE_Thread_Mutex, g, lock_);
    RtpsReaderMap::iterator rr = readers_.find(local_id);

    if (rr != readers_.end()) {
      for (pair<RtpsReaderMultiMap::iterator, RtpsReaderMultiMap::iterator> iters =
             readers_of_writer_.equal_range(remote_id);
           iters.first != iters.second;) {
        if (iters.first->first == local_id) {
          readers_of_writer_.erase(iters.first++);
        } else {
          ++iters.first;
        }
      }

      RtpsReader_rch reader = rr->second;
      g.release();
      reader->remove_writer(remote_id);

      if (reader->writer_count() == 0) {
        {
          ACE_GUARD(ACE_Thread_Mutex, h, lock_);
          rr = readers_.find(local_id);
          if (rr != readers_.end()) {
            readers_.erase(rr);
          }
        }
      }
    }
  }

  typedef OPENDDS_VECTOR(TransportQueueElement*)::iterator tqe_iter;
  tqe_iter deliver_it = to_deliver.begin();
  while (deliver_it != to_deliver.end()) {
    (*deliver_it)->data_delivered();
    ++deliver_it;
  }
  tqe_iter drop_it = to_drop.begin();
  while (drop_it != to_drop.end()) {
    (*drop_it)->data_dropped(true);
    ++drop_it;
  }
}

void
RtpsUdpDataLink::stop_i()
{
  nack_reply_.cancel();
  heartbeat_reply_.cancel();
  heartbeat_->disable();
  heartbeatchecker_->disable();
  relay_beacon_->disable();
  unicast_socket_.close();
  multicast_socket_.close();
}

void
RtpsUdpDataLink::RtpsWriter::retain_all_helper(const RepoId& pub_id)
{
  ACE_GUARD(ACE_Thread_Mutex, g, mutex_);
  if (!send_buff_.is_nil()) {
    send_buff_->retain_all(pub_id);
  }
}

// Implementing MultiSendBuffer nested class

void
RtpsUdpDataLink::MultiSendBuffer::retain_all(const RepoId& pub_id)
{
  ACE_GUARD(ACE_Thread_Mutex, g, outer_->lock_);
  const RtpsWriterMap::iterator wi = outer_->writers_.find(pub_id);
  if (wi != outer_->writers_.end()) {
    wi->second->retain_all_helper(pub_id);
  }
}

void
RtpsUdpDataLink::RtpsWriter::msb_insert_helper(const TransportQueueElement* const tqe,
                                               const SequenceNumber& seq,
                                               TransportSendStrategy::QueueType* q,
                                               ACE_Message_Block* chain)
{
  ACE_GUARD(ACE_Thread_Mutex, g, mutex_);

  RtpsUdpDataLink_rch link = link_.lock();
  if (!link) {
    return;
  }

  const RepoId pub_id = tqe->publication_id();

  if (send_buff_.is_nil()) {
    send_buff_ = make_rch<SingleSendBuffer>(SingleSendBuffer::UNLIMITED, 1 /*mspp*/);

    send_buff_->bind(link->send_strategy());
  }

  if (Transport_debug_level > 5) {
    const GuidConverter pub(pub_id);
    ACE_DEBUG((LM_DEBUG, "(%P|%t) RtpsUdpDataLink::MultiSendBuffer::insert() - "
      "pub_id %C seq %q frag %d\n", OPENDDS_STRING(pub).c_str(), seq.getValue(),
      (int)tqe->is_fragment()));
  }

  if (tqe->is_fragment()) {
    const RtpsCustomizedElement* const rce =
      dynamic_cast<const RtpsCustomizedElement*>(tqe);
    if (rce) {
      send_buff_->insert_fragment(seq, rce->last_fragment(), q, chain);
    } else if (Transport_debug_level) {
      const GuidConverter pub(pub_id);
      ACE_ERROR((LM_ERROR, "(%P|%t) RtpsUdpDataLink::MultiSendBuffer::insert()"
        " - ERROR: couldn't get fragment number for pub_id %C seq %q\n",
        OPENDDS_STRING(pub).c_str(), seq.getValue()));
    }
  } else {
    send_buff_->insert(seq, q, chain);
  }
}

void
RtpsUdpDataLink::MultiSendBuffer::insert(SequenceNumber /*transport_seq*/,
                                         TransportSendStrategy::QueueType* q,
                                         ACE_Message_Block* chain)
{
  // Called from TransportSendStrategy::send_packet().
  // RtpsUdpDataLink is already locked.
  const TransportQueueElement* const tqe = q->peek();
  const SequenceNumber seq = tqe->sequence();
  if (seq == SequenceNumber::SEQUENCENUMBER_UNKNOWN()) {
    return;
  }

  const RepoId pub_id = tqe->publication_id();

  const RtpsWriterMap::iterator wi = outer_->writers_.find(pub_id);
  if (wi == outer_->writers_.end()) {
    return; // this datawriter is not reliable
  }
  RtpsWriter_rch writer = wi->second;
  writer->msb_insert_helper(tqe, seq, q, chain);
}

// Support for the send() data handling path
namespace {
  ACE_Message_Block* submsgs_to_msgblock(const RTPS::SubmessageSeq& subm)
  {
    size_t size = 0, padding = 0;
    for (CORBA::ULong i = 0; i < subm.length(); ++i) {
      if ((size + padding) % 4) {
        padding += 4 - ((size + padding) % 4);
      }
      gen_find_size(subm[i], size, padding);
    }

    ACE_Message_Block* hdr = new ACE_Message_Block(size + padding);

    for (CORBA::ULong i = 0; i < subm.length(); ++i) {
      // byte swapping is handled in the operator<<() implementation
      Serializer ser(hdr, false, Serializer::ALIGN_CDR);
      ser << subm[i];
      const size_t len = hdr->length();
      if (len % 4) {
        hdr->wr_ptr(4 - (len % 4));
      }
    }
    return hdr;
  }
}

TransportQueueElement*
RtpsUdpDataLink::RtpsWriter::customize_queue_element_helper(TransportQueueElement* element, bool requires_inline_qos, MetaSubmessageVec& meta_submessages, bool& deliver_after_send)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, 0);

  RtpsUdpDataLink_rch link = link_.lock();
  if (!link) {
    return 0;
  }

  bool gap_ok = true;
  DestToEntityMap gap_receivers;
  if (!remote_readers_.empty()) {
    for (ReaderInfoMap::iterator ri = remote_readers_.begin();
         ri != remote_readers_.end(); ++ri) {
      RepoId tmp;
      std::memcpy(tmp.guidPrefix, ri->first.guidPrefix, sizeof(GuidPrefix_t));
      tmp.entityId = ENTITYID_UNKNOWN;
      gap_receivers[tmp].push_back(ri->first);

      if (ri->second.expecting_durable_data()) {
        // Can't add an in-line GAP if some Data Reader is expecting durable
        // data, the GAP could cause that Data Reader to ignore the durable
        // data.  The other readers will eventually learn about the GAP by
        // sending an ACKNACK and getting a GAP reply.
        gap_ok = false;
        break;
      }
    }
  }

  RTPS::SubmessageSeq subm;

  if (gap_ok) {
    add_gap_submsg_i(subm, *element, gap_receivers);
  }

  const SequenceNumber seq = element->sequence();
  if (seq != SequenceNumber::SEQUENCENUMBER_UNKNOWN()) {
    expected_ = seq;
    ++expected_;
  }

  TransportSendElement* tse = dynamic_cast<TransportSendElement*>(element);
  TransportCustomizedElement* tce =
    dynamic_cast<TransportCustomizedElement*>(element);
  TransportSendControlElement* tsce =
    dynamic_cast<TransportSendControlElement*>(element);

  Message_Block_Ptr data;
  bool durable = false;

  const ACE_Message_Block* msg = element->msg();
  const RepoId pub_id = element->publication_id();

  // Based on the type of 'element', find and duplicate the data payload
  // continuation block.
  if (tsce) {        // Control message
    if (RtpsSampleHeader::control_message_supported(tsce->header().message_id_)) {
      data.reset(msg->cont()->duplicate());
      // Create RTPS Submessage(s) in place of the OpenDDS DataSampleHeader
      RtpsSampleHeader::populate_data_control_submessages(
                subm, *tsce, requires_inline_qos);
    } else if (tsce->header().message_id_ == END_HISTORIC_SAMPLES) {
      end_historic_samples_i(tsce->header(), msg->cont());
      element->data_delivered();
      return 0;
    } else if (tsce->header().message_id_ == DATAWRITER_LIVELINESS) {
      send_heartbeats_manual_i(meta_submessages);
      deliver_after_send = true;
      return 0;
    } else {
      element->data_dropped(true /*dropped_by_transport*/);
      return 0;
    }

  } else if (tse) {  // Basic data message
    // {DataSampleHeader} -> {Data Payload}
    data.reset(msg->cont()->duplicate());
    const DataSampleElement* dsle = tse->sample();
    // Create RTPS Submessage(s) in place of the OpenDDS DataSampleHeader
    RtpsSampleHeader::populate_data_sample_submessages(
              subm, *dsle, requires_inline_qos);
    durable = dsle->get_header().historic_sample_;

  } else if (tce) {  // Customized data message
    // {DataSampleHeader} -> {Content Filtering GUIDs} -> {Data Payload}
    data.reset(msg->cont()->cont()->duplicate());
    const DataSampleElement* dsle = tce->original_send_element()->sample();
    // Create RTPS Submessage(s) in place of the OpenDDS DataSampleHeader
    RtpsSampleHeader::populate_data_sample_submessages(
              subm, *dsle, requires_inline_qos);
    durable = dsle->get_header().historic_sample_;

  } else {
    return element;
  }

#ifdef OPENDDS_SECURITY
  {
    GuardType guard(link->strategy_lock_);
    if (link->send_strategy_) {
      link->send_strategy()->encode_payload(pub_id, data, subm);
    }
  }
#endif

  Message_Block_Ptr hdr(submsgs_to_msgblock(subm));
  hdr->cont(data.release());
  RtpsCustomizedElement* rtps =
    new RtpsCustomizedElement(element, move(hdr));

  // Handle durability resends
  if (durable) {
    const RepoId sub = element->subscription_id();
    if (sub != GUID_UNKNOWN) {
      ReaderInfoMap::iterator ri = remote_readers_.find(sub);
      if (ri != remote_readers_.end()) {
        ri->second.durable_data_[rtps->sequence()] = rtps;
        ri->second.durable_timestamp_ = ACE_OS::gettimeofday();
        if (Transport_debug_level > 3) {
          const GuidConverter conv(pub_id), sub_conv(sub);
          ACE_DEBUG((LM_DEBUG,
            "(%P|%t) RtpsUdpDataLink::customize_queue_element() - "
            "storing durable data for local %C remote %C seq %q\n",
            OPENDDS_STRING(conv).c_str(), OPENDDS_STRING(sub_conv).c_str(),
            rtps->sequence().getValue()));
        }
        return 0;
      }
    }
  } else if (durable && (Transport_debug_level)) {
    const GuidConverter conv(pub_id);
    ACE_ERROR((LM_ERROR,
      "(%P|%t) RtpsUdpDataLink::customize_queue_element() - "
      "WARNING: no RtpsWriter to store durable data for local %C\n",
      OPENDDS_STRING(conv).c_str()));
  }

  return rtps;
}

TransportQueueElement*
RtpsUdpDataLink::customize_queue_element_non_reliable_i(TransportQueueElement* element, bool requires_inline_qos, MetaSubmessageVec& meta_submessages, bool& deliver_after_send)
{
  RTPS::SubmessageSeq subm;

  TransportSendElement* tse = dynamic_cast<TransportSendElement*>(element);
  TransportCustomizedElement* tce =
    dynamic_cast<TransportCustomizedElement*>(element);
  TransportSendControlElement* tsce =
    dynamic_cast<TransportSendControlElement*>(element);

  Message_Block_Ptr data;

  const ACE_Message_Block* msg = element->msg();

  // Based on the type of 'element', find and duplicate the data payload
  // continuation block.
  if (tsce) {        // Control message
    if (RtpsSampleHeader::control_message_supported(tsce->header().message_id_)) {
      data.reset(msg->cont()->duplicate());
      // Create RTPS Submessage(s) in place of the OpenDDS DataSampleHeader
      RtpsSampleHeader::populate_data_control_submessages(
                subm, *tsce, requires_inline_qos);
    } else if (tsce->header().message_id_ == DATAWRITER_LIVELINESS) {
      send_heartbeats_manual_i(tsce, meta_submessages);
      deliver_after_send = true;
      return 0;
    } else {
      element->data_dropped(true /*dropped_by_transport*/);
      return 0;
    }

  } else if (tse) {  // Basic data message
    // {DataSampleHeader} -> {Data Payload}
    data.reset(msg->cont()->duplicate());
    const DataSampleElement* dsle = tse->sample();
    // Create RTPS Submessage(s) in place of the OpenDDS DataSampleHeader
    RtpsSampleHeader::populate_data_sample_submessages(
              subm, *dsle, requires_inline_qos);

  } else if (tce) {  // Customized data message
    // {DataSampleHeader} -> {Content Filtering GUIDs} -> {Data Payload}
    data.reset(msg->cont()->cont()->duplicate());
    const DataSampleElement* dsle = tce->original_send_element()->sample();
    // Create RTPS Submessage(s) in place of the OpenDDS DataSampleHeader
    RtpsSampleHeader::populate_data_sample_submessages(
              subm, *dsle, requires_inline_qos);

  } else {
    return element;
  }

#ifdef OPENDDS_SECURITY
  const RepoId pub_id = element->publication_id();

  {
    GuardType guard(this->strategy_lock_);
    if (this->send_strategy_) {
      send_strategy()->encode_payload(pub_id, data, subm);
    }
  }
#endif

  Message_Block_Ptr hdr(submsgs_to_msgblock(subm));
  hdr->cont(data.release());
  return new RtpsCustomizedElement(element, move(hdr));
}

TransportQueueElement*
RtpsUdpDataLink::customize_queue_element(TransportQueueElement* element)
{
  const ACE_Message_Block* msg = element->msg();
  if (!msg) {
    return element;
  }

  const RepoId pub_id = element->publication_id();
  GUIDSeq_var peers = peer_ids(pub_id);

  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, lock_, 0);

  bool requires_inline_qos = this->requires_inline_qos(peers);

  const RtpsWriterMap::iterator rw = writers_.find(pub_id);
  MetaSubmessageVec meta_submessages;
  RtpsWriter_rch writer;
  TransportQueueElement* result;
  bool deliver_after_send = false;
  if (rw != writers_.end()) {
    g.release();
    result = rw->second->customize_queue_element_helper(element, requires_inline_qos, meta_submessages, deliver_after_send);
  } else {
    result = customize_queue_element_non_reliable_i(element, requires_inline_qos, meta_submessages, deliver_after_send);
    g.release();
  }

  send_bundled_submessages(meta_submessages);

  if (deliver_after_send) {
    element->data_delivered();
  }

  return result;
}

void
RtpsUdpDataLink::RtpsWriter::end_historic_samples_i(const DataSampleHeader& header,
                                                    ACE_Message_Block* body)
{
  // Set the ReaderInfo::durable_timestamp_ for the case where no
  // durable samples exist in the DataWriter.
  if (durable_) {
    const ACE_Time_Value now = ACE_OS::gettimeofday();
    RepoId sub = GUID_UNKNOWN;
    if (body && header.message_length_ >= sizeof(sub)) {
      std::memcpy(&sub, body->rd_ptr(), header.message_length_);
    }
    typedef ReaderInfoMap::iterator iter_t;
    if (sub == GUID_UNKNOWN) {
      if (Transport_debug_level > 3) {
        const GuidConverter conv(id_);
        ACE_DEBUG((LM_DEBUG, "(%P|%t) RtpsUdpDataLink::end_historic_samples "
                   "local %C all readers\n", OPENDDS_STRING(conv).c_str()));
      }
      for (iter_t iter = remote_readers_.begin();
           iter != remote_readers_.end(); ++iter) {
        if (iter->second.durable_) {
          iter->second.durable_timestamp_ = now;
        }
      }
    } else {
      iter_t iter = remote_readers_.find(sub);
      if (iter != remote_readers_.end()) {
        if (iter->second.durable_) {
          iter->second.durable_timestamp_ = now;
          if (Transport_debug_level > 3) {
            const GuidConverter conv(id_), sub_conv(sub);
            ACE_DEBUG((LM_DEBUG, "(%P|%t) RtpsUdpDataLink::end_historic_samples"
                       " local %C remote %C\n", OPENDDS_STRING(conv).c_str(),
                       OPENDDS_STRING(sub_conv).c_str()));
          }
        }
      }
    }

    // This should always succeed, since this method is called by customize_queue_element_helper,
    // which already holds a RCH to the datalink... this is just to avoid adding another parameter to pass it
    RtpsUdpDataLink_rch link = link_.lock();
    if (link) {
      link->heartbeat_->schedule_enable(true);
    }
  }
}

bool RtpsUdpDataLink::requires_inline_qos(const GUIDSeq_var& peers)
{
  if (force_inline_qos_) {
    // Force true for testing purposes
    return true;
  } else {
    if (!peers.ptr()) {
      return false;
    }
    for (CORBA::ULong i = 0; i < peers->length(); ++i) {
      const RemoteInfoMap::const_iterator iter = locators_.find(peers[i]);
      if (iter != locators_.end() && iter->second.requires_inline_qos_) {
        return true;
      }
    }
    return false;
  }
}

bool RtpsUdpDataLink::force_inline_qos_ = false;

void
RtpsUdpDataLink::RtpsWriter::add_gap_submsg_i(RTPS::SubmessageSeq& msg,
                                              const TransportQueueElement& tqe,
                                              const DestToEntityMap& dtem)
{
  // These are the GAP submessages that we'll send directly in-line with the
  // DATA when we notice that the DataWriter has deliberately skipped seq #s.
  // There are other GAP submessages generated in meta_submessage to reader ACKNACKS,
  // see send_nack_replies().
  using namespace OpenDDS::RTPS;

  const SequenceNumber seq = tqe.sequence();
  const RepoId pub = tqe.publication_id();
  if (seq == SequenceNumber::SEQUENCENUMBER_UNKNOWN() || pub == GUID_UNKNOWN
      || tqe.subscription_id() != GUID_UNKNOWN) {
    return;
  }

  if (seq != expected_) {
    SequenceNumber firstMissing = expected_;

    // RTPS v2.1 8.3.7.4: the Gap sequence numbers are those in the range
    // [gapStart, gapListBase) and those in the SNSet.
    const SequenceNumber_t gapStart = {firstMissing.getHigh(),
                                       firstMissing.getLow()},
                           gapListBase = {seq.getHigh(),
                                          seq.getLow()};

    // We are not going to enable any bits in the "bitmap" of the SNSet,
    // but the "numBits" and the bitmap.length must both be > 0.
    LongSeq8 bitmap;
    bitmap.length(1);
    bitmap[0] = 0;

    GapSubmessage gap = {
      {GAP, FLAG_E, 0 /*length determined below*/},
      ENTITYID_UNKNOWN, // readerId: applies to all matched readers
      pub.entityId,
      gapStart,
      {gapListBase, 1, bitmap}
    };

    size_t size = 0, padding = 0;
    gen_find_size(gap, size, padding);
    gap.smHeader.submessageLength =
      static_cast<CORBA::UShort>(size + padding) - SMHDR_SZ;

    if (!durable_) {
      const CORBA::ULong i = msg.length();
      msg.length(i + 1);
      msg[i].gap_sm(gap);
    } else {
      InfoDestinationSubmessage idst = {
        {INFO_DST, FLAG_E, INFO_DST_SZ},
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
      };
      CORBA::ULong ml = msg.length();

      //Change the non-directed Gap into multiple directed gaps to prevent
      //delivering to currently undiscovered durable readers
      DestToEntityMap::const_iterator iter = dtem.begin();
      while (iter != dtem.end()) {
        std::memcpy(idst.guidPrefix, iter->first.guidPrefix, sizeof(GuidPrefix_t));
        msg.length(ml + 1);
        msg[ml++].info_dst_sm(idst);

        const OPENDDS_VECTOR(RepoId)& readers = iter->second;
        for (size_t i = 0; i < readers.size(); ++i) {
          gap.readerId = readers.at(i).entityId;
          msg.length(ml + 1);
          msg[ml++].gap_sm(gap);
        } //END iter over reader entity ids
        ++iter;
      } //END iter over reader GuidPrefix_t's
    }
  }
}


// DataReader's side of Reliability

void
RtpsUdpDataLink::received(const RTPS::DataSubmessage& data,
                          const GuidPrefix_t& src_prefix)
{
  datareader_dispatch(data, src_prefix, &RtpsReader::process_data_i);
}

bool
RtpsUdpDataLink::RtpsReader::process_data_i(const RTPS::DataSubmessage& data,
                                            const RepoId& src,
                                            MetaSubmessageVec&)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  RtpsUdpDataLink_rch link = link_.lock();
  const WriterInfoMap::iterator wi = remote_writers_.find(src);
  if (wi != remote_writers_.end() && link) {
    WriterInfo& info = wi->second;
    SequenceNumber seq;
    seq.setValue(data.writerSN.high, data.writerSN.low);
    info.frags_.erase(seq);
    if (info.recvd_.contains(seq)) {
      if (Transport_debug_level > 5) {
        GuidConverter writer(src);
        GuidConverter reader(id_);
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) RtpsUdpDataLink::process_data_i(DataSubmessage) -")
                             ACE_TEXT(" data seq: %q from %C being WITHHELD from %C because ALREADY received\n"),
                             seq.getValue(),
                             OPENDDS_STRING(writer).c_str(),
                             OPENDDS_STRING(reader).c_str()));
      }
      link->receive_strategy()->withhold_data_from(id_);
    } else if (info.recvd_.disjoint() ||
        (!info.recvd_.empty() && info.recvd_.cumulative_ack() != seq.previous())
        || (durable_ && !info.recvd_.empty() && info.recvd_.low() > 1)
        || (durable_ && info.recvd_.empty() && seq > 1)) {
      if (Transport_debug_level > 5) {
        GuidConverter writer(src);
        GuidConverter reader(id_);
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) RtpsUdpDataLink::process_data_i(DataSubmessage) -")
                             ACE_TEXT(" data seq: %q from %C being WITHHELD from %C because can't receive yet\n"),
                             seq.getValue(),
                             OPENDDS_STRING(writer).c_str(),
                             OPENDDS_STRING(reader).c_str()));
      }
      const ReceivedDataSample* sample =
        link->receive_strategy()->withhold_data_from(id_);
      info.held_.insert(std::make_pair(seq, *sample));
    } else {
      if (Transport_debug_level > 5) {
        GuidConverter writer(src);
        GuidConverter reader(id_);
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) RtpsUdpDataLink::process_data_i(DataSubmessage) -")
                             ACE_TEXT(" data seq: %q from %C to %C OK to deliver\n"),
                             seq.getValue(),
                             OPENDDS_STRING(writer).c_str(),
                             OPENDDS_STRING(reader).c_str()));
      }
      link->receive_strategy()->do_not_withhold_data_from(id_);
    }
    info.recvd_.insert(seq);
    link->deliver_held_data(id_, info, durable_);
  } else if (link) {
    if (Transport_debug_level > 5) {
      GuidConverter writer(src);
      GuidConverter reader(id_);
      SequenceNumber seq;
      seq.setValue(data.writerSN.high, data.writerSN.low);
      ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) RtpsUdpDataLink::process_data_i(DataSubmessage) -")
                           ACE_TEXT(" data seq: %q from %C to %C OK to deliver (Writer not currently in Reader remote writer map)\n"),
                           seq.getValue(),
                           OPENDDS_STRING(writer).c_str(),
                           OPENDDS_STRING(reader).c_str()));
    }
    link->receive_strategy()->do_not_withhold_data_from(id_);
  }
  return false;
}

void
RtpsUdpDataLink::deliver_held_data(const RepoId& readerId, WriterInfo& info,
                                   bool durable)
{
  if (durable && (info.recvd_.empty() || info.recvd_.low() > 1)) return;
  held_data_delivery_handler_.notify_delivery(readerId, info);
}

void
RtpsUdpDataLink::received(const RTPS::GapSubmessage& gap,
                          const GuidPrefix_t& src_prefix)
{
  datareader_dispatch(gap, src_prefix, &RtpsReader::process_gap_i);
}

bool
RtpsUdpDataLink::RtpsReader::process_gap_i(const RTPS::GapSubmessage& gap,
                                           const RepoId& src,
                                           MetaSubmessageVec&)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  RtpsUdpDataLink_rch link = link_.lock();
  const WriterInfoMap::iterator wi = remote_writers_.find(src);
  if (wi != remote_writers_.end() && link) {
    SequenceRange sr;
    sr.first.setValue(gap.gapStart.high, gap.gapStart.low);
    SequenceNumber base;
    base.setValue(gap.gapList.bitmapBase.high, gap.gapList.bitmapBase.low);
    SequenceNumber first_received = SequenceNumber::MAX_VALUE;
    if (!wi->second.recvd_.empty()) {
      OPENDDS_VECTOR(SequenceRange) missing = wi->second.recvd_.missing_sequence_ranges();
      if (!missing.empty()) {
        first_received = missing.front().second;
      }
    }
    sr.second = std::min(first_received, base.previous());
    if (sr.first <= sr.second) {
      if (Transport_debug_level > 5) {
        const GuidConverter conv(src);
        const GuidConverter rdr(id_);
        ACE_DEBUG((LM_DEBUG, "(%P|%t) RtpsUdpDataLink::process_gap_i "
                  "Reader %C received GAP with range [%q, %q] (inserting range [%q, %q]) from %C\n",
                  OPENDDS_STRING(rdr).c_str(),
                  sr.first.getValue(), base.previous().getValue(),
                  sr.first.getValue(), sr.second.getValue(),
                  OPENDDS_STRING(conv).c_str()));
      }
      wi->second.recvd_.insert(sr);
    } else {
      const GuidConverter conv(src);
      VDBG_LVL((LM_WARNING, "(%P|%t) RtpsUdpDataLink::process_gap_i "
                "received GAP with invalid range [%q, %q] from %C\n",
                sr.first.getValue(), sr.second.getValue(),
                OPENDDS_STRING(conv).c_str()), 2);
    }
    wi->second.recvd_.insert(base, gap.gapList.numBits,
                             gap.gapList.bitmap.get_buffer());
    link->deliver_held_data(id_, wi->second, durable_);
    //FUTURE: to support wait_for_acks(), notify DCPS layer of the GAP
  }
  return false;
}

void
RtpsUdpDataLink::received(const RTPS::HeartBeatSubmessage& heartbeat,
                          const GuidPrefix_t& src_prefix)
{
  RepoId src;
  std::memcpy(src.guidPrefix, src_prefix, sizeof(GuidPrefix_t));
  src.entityId = heartbeat.writerId;

  bool schedule_acknack = false;
  const ACE_Time_Value now = ACE_OS::gettimeofday();
  OPENDDS_VECTOR(InterestingRemote) callbacks;

  {
    ACE_GUARD(ACE_Thread_Mutex, g, lock_);

    // We received a heartbeat from a writer.
    // We should ACKNACK if the writer is interesting and there is no association.

    for (InterestingRemoteMapType::iterator pos = interesting_writers_.lower_bound(src),
           limit = interesting_writers_.upper_bound(src);
         pos != limit;
         ++pos) {
      const RepoId& writerid = src;
      const RepoId& readerid = pos->second.localid;

      RtpsReaderMap::const_iterator riter = readers_.find(readerid);
      if (riter == readers_.end()) {
        // Reader has no associations.
        interesting_ack_nacks_.insert(InterestingAckNack(writerid, readerid, pos->second.address));
      } else if (riter->second->has_writer(writerid)) {
        // Reader is not associated with this writer.
        interesting_ack_nacks_.insert(InterestingAckNack(writerid, readerid, pos->second.address));
      }
      pos->second.last_activity = now;
      if (pos->second.status == InterestingRemote::DOES_NOT_EXIST) {
        callbacks.push_back(pos->second);
        pos->second.status = InterestingRemote::EXISTS;
      }
    }

    schedule_acknack = !interesting_ack_nacks_.empty();
  }

  for (size_t i = 0; i < callbacks.size(); ++i) {
    callbacks[i].listener->writer_exists(src, callbacks[i].localid);
  }

  if (schedule_acknack) {
    heartbeat_reply_.schedule();
  }

  datareader_dispatch(heartbeat, src_prefix,
                      &RtpsReader::process_heartbeat_i);
}

bool
RtpsUdpDataLink::RtpsReader::process_heartbeat_i(const RTPS::HeartBeatSubmessage& heartbeat,
                                                 const RepoId& src,
                                                 MetaSubmessageVec& meta_submessages)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  RtpsUdpDataLink_rch link = link_.lock();
  const WriterInfoMap::iterator wi = remote_writers_.find(src);
  if (wi == remote_writers_.end() || !link) {
    // we may not be associated yet, even if the writer thinks we are
    return false;
  }

  WriterInfo& info = wi->second;

  if (heartbeat.count.value <= info.heartbeat_recvd_count_) {
    return false;
  }

  bool immediate_reply = false;
  SequenceNumber hb_first;
  hb_first.setValue(heartbeat.firstSN.high, heartbeat.firstSN.low);
  SequenceNumber hb_last;
  hb_last.setValue(heartbeat.lastSN.high, heartbeat.lastSN.low);
  if (info.hb_range_.second.getValue() == 0 && hb_last.getValue() != 0) {
    immediate_reply = true;
  }
  info.heartbeat_recvd_count_ = heartbeat.count.value;

  SequenceNumber& first = info.hb_range_.first;
  first.setValue(heartbeat.firstSN.high, heartbeat.firstSN.low);
  SequenceNumber& last = info.hb_range_.second;
  last.setValue(heartbeat.lastSN.high, heartbeat.lastSN.low);
  static const SequenceNumber starting, zero = SequenceNumber::ZERO();

  // Only 'apply' heartbeat ranges to received set if the heartbeat is valid
  // But for the sake of speedy discovery / association we'll still respond to invalid non-final heartbeats
  if (last.getValue() >= starting.getValue()) {

    DisjointSequence& recvd = info.recvd_;
    if (!durable_ && info.initial_hb_) {
      // For the non-durable reader, the first received HB or DATA establishes
      // a baseline of the lowest sequence number we'd ever need to NACK.
      if (recvd.empty() || recvd.low() >= last) {
        recvd.insert(SequenceRange(zero, last));
      } else {
        recvd.insert(SequenceRange(zero, recvd.low()));
      }
    } else if (!recvd.empty()) {
      // All sequence numbers below 'first' should not be NACKed.
      // The value of 'first' may not decrease with subsequent HBs.
      recvd.insert(SequenceRange(zero,
                                 (first > starting) ? first.previous() : zero));
    }

    link->deliver_held_data(id_, info, durable_);

    //FUTURE: to support wait_for_acks(), notify DCPS layer of the sequence
    //        numbers we no longer expect to receive due to HEARTBEAT

    info.initial_hb_ = false;
  }

  const bool is_final = heartbeat.smHeader.flags & RTPS::FLAG_F,
    liveliness = heartbeat.smHeader.flags & RTPS::FLAG_L;

  if (!is_final || (!liveliness && (info.should_nack() ||
      should_nack_durable(info) ||
      link->receive_strategy()->has_fragments(info.hb_range_, wi->first)))) {
    info.ack_pending_ = true;

    if (immediate_reply) {
      gather_ack_nacks_i(meta_submessages);
      return false;
    } else {
      return true; // timer will invoke send_heartbeat_replies()
    }
  }

  //FUTURE: support assertion of liveliness for MANUAL_BY_TOPIC
  return false;
}

bool
RtpsUdpDataLink::WriterInfo::should_nack() const
{
  if (recvd_.disjoint() && recvd_.cumulative_ack() < hb_range_.second) {
    return true;
  }
  if (!recvd_.empty()) {
    return recvd_.high() < hb_range_.second;
  }
  return false;
}

bool
RtpsUdpDataLink::RtpsWriter::add_reader(const RepoId& id, const ReaderInfo& info)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  ReaderInfoMap::const_iterator iter = remote_readers_.find(id);
  if (iter == remote_readers_.end()) {
    remote_readers_.insert(ReaderInfoMap::value_type(id, info));
    return true;
  }
  return false;
}

bool
RtpsUdpDataLink::RtpsWriter::has_reader(const RepoId& id) const
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  return remote_readers_.count(id) != 0;
}

bool
RtpsUdpDataLink::RtpsWriter::remove_reader(const RepoId& id)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  return remote_readers_.erase(id) > 0;
}

size_t
RtpsUdpDataLink::RtpsWriter::reader_count() const
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, 0);
  return remote_readers_.size();
}

bool
RtpsUdpDataLink::RtpsWriter::is_reader_handshake_done(const RepoId& id) const
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  ReaderInfoMap::const_iterator iter = remote_readers_.find(id);
  return iter != remote_readers_.end() && iter->second.handshake_done_;
}


bool
RtpsUdpDataLink::RtpsReader::add_writer(const RepoId& id, const WriterInfo& info)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  WriterInfoMap::const_iterator iter = remote_writers_.find(id);
  if (iter == remote_writers_.end()) {
    remote_writers_[id] = info;
    return true;
  }
  return false;
}

bool
RtpsUdpDataLink::RtpsReader::has_writer(const RepoId& id) const
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  return remote_writers_.count(id) != 0;
}

bool
RtpsUdpDataLink::RtpsReader::remove_writer(const RepoId& id)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);
  return remote_writers_.erase(id) > 0;
}

size_t
RtpsUdpDataLink::RtpsReader::writer_count() const
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, 0);
  return remote_writers_.size();
}

bool
RtpsUdpDataLink::RtpsReader::should_nack_durable(const WriterInfo& info)
{
  return durable_ && (info.recvd_.empty() || info.recvd_.low() > info.hb_range_.first);
}

void
RtpsUdpDataLink::RtpsReader::gather_ack_nacks(MetaSubmessageVec& meta_submessages, bool finalFlag)
{
  ACE_GUARD(ACE_Thread_Mutex, g, mutex_);
  gather_ack_nacks_i(meta_submessages, finalFlag);
}

void
RtpsUdpDataLink::RtpsReader::gather_ack_nacks_i(MetaSubmessageVec& meta_submessages, bool finalFlag)
{
  using namespace OpenDDS::RTPS;

  RtpsUdpDataLink_rch link = link_.lock();

  for (WriterInfoMap::iterator wi = remote_writers_.begin(); wi != remote_writers_.end(); ++wi) {

    // if we have some negative acknowledgments, we'll ask for a reply
    DisjointSequence& recvd = wi->second.recvd_;
    const bool nack = wi->second.should_nack() ||
      should_nack_durable(wi->second);
    bool is_final = finalFlag || !nack;

    if (wi->second.ack_pending_ || nack || finalFlag) {
      const bool prev_ack_pending = wi->second.ack_pending_;
      wi->second.ack_pending_ = false;

      SequenceNumber ack;
      CORBA::ULong num_bits = 1;
      LongSeq8 bitmap;
      bitmap.length(1);
      bitmap[0] = 0;

      const SequenceNumber& hb_low = wi->second.hb_range_.first;
      const SequenceNumber& hb_high = wi->second.hb_range_.second;
      const SequenceNumber::Value hb_low_val = hb_low.getValue(),
        hb_high_val = hb_high.getValue();

      if (recvd.empty()) {
        // Nack the entire heartbeat range. Only reached when durable.
        if (hb_low_val <= hb_high_val) {
          ack = hb_low;
          bitmap.length(bitmap_num_longs(ack, hb_high));
          const CORBA::ULong idx = (hb_high_val > hb_low_val + 255)
            ? 255
            : CORBA::ULong(hb_high_val - hb_low_val);
          DisjointSequence::fill_bitmap_range(0, idx,
                                              bitmap.get_buffer(),
                                              bitmap.length(), num_bits);
        }
      } else if (((prev_ack_pending && !nack) || should_nack_durable(wi->second)) && recvd.low() > hb_low) {
        // Nack the range between the heartbeat low and the recvd low.
        ack = hb_low;
        const SequenceNumber& rec_low = recvd.low();
        const SequenceNumber::Value rec_low_val = rec_low.getValue();
        bitmap.length(bitmap_num_longs(ack, rec_low));
        const CORBA::ULong idx = (rec_low_val > hb_low_val + 255)
          ? 255
          : CORBA::ULong(rec_low_val - hb_low_val);
        DisjointSequence::fill_bitmap_range(0, idx,
                                            bitmap.get_buffer(),
                                            bitmap.length(), num_bits);

      } else {
        ack = ++SequenceNumber(recvd.cumulative_ack());
        if (recvd.low().getValue() > 1) {
          // since the "ack" really is cumulative, we need to make
          // sure that a lower discontinuity is not possible later
          recvd.insert(SequenceRange(SequenceNumber::ZERO(), recvd.low()));
        }

        if (recvd.disjoint()) {
          bitmap.length(bitmap_num_longs(ack, recvd.last_ack().previous()));
          recvd.to_bitmap(bitmap.get_buffer(), bitmap.length(),
                          num_bits, true);
        }
      }

      const SequenceNumber::Value ack_val = ack.getValue();

      if (!recvd.empty() && hb_high > recvd.high()) {
        const SequenceNumber eff_high =
          (hb_high <= ack_val + 255) ? hb_high : (ack_val + 255);
        const SequenceNumber::Value eff_high_val = eff_high.getValue();
        // Nack the range between the received high and the effective high.
        const CORBA::ULong old_len = bitmap.length(),
          new_len = bitmap_num_longs(ack, eff_high);
        if (new_len > old_len) {
          bitmap.length(new_len);
          for (CORBA::ULong i = old_len; i < new_len; ++i) {
            bitmap[i] = 0;
          }
        }
        const CORBA::ULong idx_hb_high = CORBA::ULong(eff_high_val - ack_val),
          idx_recv_high = recvd.disjoint() ?
          CORBA::ULong(recvd.high().getValue() - ack_val) : 0;
        DisjointSequence::fill_bitmap_range(idx_recv_high, idx_hb_high,
                                            bitmap.get_buffer(), new_len,
                                            num_bits);
      }

      // If the receive strategy is holding any fragments, those should
      // not be "nacked" in the ACKNACK reply.  They will be accounted for
      // in the NACK_FRAG(s) instead.
      bool frags_modified =
        link->receive_strategy()->remove_frags_from_bitmap(bitmap.get_buffer(),
                                                 num_bits, ack, wi->first);
      if (frags_modified && !is_final) { // change to is_final if bitmap is empty
        is_final = true;
        for (CORBA::ULong i = 0; i < bitmap.length(); ++i) {
          if ((i + 1) * 32 <= num_bits) {
            if (bitmap[i]) {
              is_final = false;
              break;
            }
          } else {
            if ((0xffffffff << (32 - (num_bits % 32))) & bitmap[i]) {
              is_final = false;
              break;
            }
          }
        }
      }

      EntityId_t reader_id = id_.entityId, writer_id = wi->first.entityId;

      MetaSubmessage meta_submessage(id_, wi->first);

      AckNackSubmessage acknack = {
        {ACKNACK,
         CORBA::Octet(FLAG_E | (is_final ? FLAG_F : 0)),
         0 /*length*/},
        id_.entityId,
        wi->first.entityId,
        { // SequenceNumberSet: acking bitmapBase - 1
          {ack.getHigh(), ack.getLow()},
          num_bits, bitmap
        },
        {++wi->second.acknack_count_}
      };
      meta_submessage.sm_.acknack_sm(acknack);

      meta_submessages.push_back(meta_submessage);

      NackFragSubmessageVec nfsv;
      generate_nack_frags(nfsv, wi->second, wi->first);
      for (size_t i = 0; i < nfsv.size(); ++i) {
        nfsv[i].readerId = reader_id;
        nfsv[i].writerId = writer_id;
        meta_submessage.sm_.nack_frag_sm(nfsv[i]);
        meta_submessages.push_back(meta_submessage);
      }
    }
  }
}

void
RtpsUdpDataLink::build_meta_submessage_map(MetaSubmessageVec& meta_submessages, AddrDestMetaSubmessageMap& adr_map)
{
  ACE_GUARD(ACE_Thread_Mutex, g, lock_);
  AddrSet addrs;
  // Sort meta_submessages by address set and destination
  for (MetaSubmessageVec::iterator it = meta_submessages.begin(); it != meta_submessages.end(); ++it) {
    if (it->dst_guid_ == GUID_UNKNOWN) {
      addrs = get_addresses_i(it->from_guid_); // This will overwrite, but addrs should always be empty here
    } else {
      accumulate_addresses(it->from_guid_, it->dst_guid_, addrs);
    }
    for (RepoIdSet::iterator it2 = it->to_guids_.begin(); it2 != it->to_guids_.end(); ++it2) {
      accumulate_addresses(it->from_guid_, *it2, addrs);
    }
    if (addrs.empty()) {
      continue;
    }
    if (std::memcmp(&(it->dst_guid_.guidPrefix), &GUIDPREFIX_UNKNOWN, sizeof(GuidPrefix_t)) != 0) {
      RepoId dst;
      std::memcpy(dst.guidPrefix, it->dst_guid_.guidPrefix, sizeof(dst.guidPrefix));
      dst.entityId = ENTITYID_UNKNOWN;
      adr_map[addrs][dst].push_back(it);
    } else {
      adr_map[addrs][GUID_UNKNOWN].push_back(it);
    }
    addrs.clear();
  }
}

namespace {

struct BundleHelper {
  BundleHelper(size_t max_bundle_size, OPENDDS_VECTOR(size_t)& meta_submessage_bundle_sizes) : max_bundle_size_(max_bundle_size), size_(0), padding_(0), prev_size_(0), prev_padding_(0), meta_submessage_bundle_sizes_(meta_submessage_bundle_sizes) {}

  void end_bundle() {
    meta_submessage_bundle_sizes_.push_back(size_ + padding_);
    size_ = 0; padding_ = 0; prev_size_ = 0; prev_padding_ = 0;
  }

  template <typename T>
  void push_to_next_bundle(const T&) {
    meta_submessage_bundle_sizes_.push_back(prev_size_ + prev_padding_);
    size_ -= prev_size_; padding_ -= prev_padding_; prev_size_ = 0; prev_padding_ = 0;
  }

  template <typename T>
  bool add_to_bundle(const T& val) {
    prev_size_ = size_;
    prev_padding_ = padding_;
    gen_find_size(val, size_, padding_);
    if ((size_ + padding_) > max_bundle_size_) {
      push_to_next_bundle(val);
      return false;
    }
    return true;
  }

  size_t prev_size_diff() const {
    return size_ - prev_size_;
  }

  size_t prev_padding_diff() const {
    return padding_ - prev_padding_;
  }

  size_t max_bundle_size_;
  size_t size_, padding_, prev_size_, prev_padding_;
  OPENDDS_VECTOR(size_t)& meta_submessage_bundle_sizes_;
};

}

void
RtpsUdpDataLink::bundle_mapped_meta_submessages(AddrDestMetaSubmessageMap& adr_map,
                                         MetaSubmessageIterVecVec& meta_submessage_bundles,
                                         OPENDDS_VECTOR(AddrSet)& meta_submessage_bundle_addrs,
                                         OPENDDS_VECTOR(size_t)& meta_submessage_bundle_sizes)
{
  using namespace RTPS;

  // Reusable INFO_DST
  InfoDestinationSubmessage idst = {
    {INFO_DST, FLAG_E, INFO_DST_SZ},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
  };

  BundleHelper helper(max_bundle_size_, meta_submessage_bundle_sizes);
  RepoId prev_dst; // used to determine when we need to write a new info_dst
  for (AddrDestMetaSubmessageMap::iterator addr_it = adr_map.begin(); addr_it != adr_map.end(); ++addr_it) {

    // A new address set always starts a new bundle
    meta_submessage_bundles.push_back(MetaSubmessageIterVec());
    meta_submessage_bundle_addrs.push_back(addr_it->first);
    prev_dst = GUID_UNKNOWN;

    for (DestMetaSubmessageMap::iterator dest_it = addr_it->second.begin(); dest_it != addr_it->second.end(); ++dest_it) {
      for (MetaSubmessageIterVec::iterator resp_it = dest_it->second.begin(); resp_it != dest_it->second.end(); ++resp_it) {
        // Check before every meta_submessage to see if we need to prefix a INFO_DST
        if (dest_it->first != GUID_UNKNOWN && dest_it->first != prev_dst) {
          // If adding an INFO_DST prefix bumped us over the limit, push the size difference into the next bundle, reset prev_dst, and keep going
          if (!helper.add_to_bundle(idst)) {
            meta_submessage_bundles.push_back(MetaSubmessageIterVec());
            meta_submessage_bundle_addrs.push_back(addr_it->first);
          }
        }
        // Attempt to add the submessage meta_submessage to the bundle
        bool result = false;
        MetaSubmessage& res = **resp_it;
        switch (res.sm_._d()) {
          case HEARTBEAT: {
            result = helper.add_to_bundle(res.sm_.heartbeat_sm());
            res.sm_.heartbeat_sm().smHeader.submessageLength = static_cast<CORBA::UShort>(helper.prev_size_diff()) - SMHDR_SZ;
            break;
          }
          case ACKNACK: {
            result = helper.add_to_bundle(res.sm_.acknack_sm());
            res.sm_.acknack_sm().smHeader.submessageLength = static_cast<CORBA::UShort>(helper.prev_size_diff()) - SMHDR_SZ;
            break;
          }
          case GAP: {
            result = helper.add_to_bundle(res.sm_.gap_sm());
            res.sm_.gap_sm().smHeader.submessageLength = static_cast<CORBA::UShort>(helper.prev_size_diff()) - SMHDR_SZ;
            break;
          }
          case NACK_FRAG: {
            result = helper.add_to_bundle(res.sm_.nack_frag_sm());
            res.sm_.nack_frag_sm().smHeader.submessageLength = static_cast<CORBA::UShort>(helper.prev_size_diff()) - SMHDR_SZ;
            break;
          }
          default: {
            break;
          }
        }
        prev_dst = dest_it->first;

        // If adding the submessage bumped us over the limit, push the size difference into the next bundle, reset prev_dst, and keep going
        if (!result) {
          meta_submessage_bundles.push_back(MetaSubmessageIterVec());
          meta_submessage_bundle_addrs.push_back(addr_it->first);
          prev_dst = GUID_UNKNOWN;
        }
        meta_submessage_bundles.back().push_back(*resp_it);
      }
    }
    helper.end_bundle();
  }
}

void
RtpsUdpDataLink::send_bundled_submessages(MetaSubmessageVec& meta_submessages)
{
  using namespace RTPS;

  if (meta_submessages.empty()) {
    return;
  }

  // Sort meta_submessages based on both locator IPs and INFO_DST GUID destination/s
  AddrDestMetaSubmessageMap adr_map;
  build_meta_submessage_map(meta_submessages, adr_map);

  // Build reasonably-sized submessage bundles based on our destination map
  MetaSubmessageIterVecVec meta_submessage_bundles; // a vector of vectors of iterators pointing to meta_submessages
  OPENDDS_VECTOR(AddrSet) meta_submessage_bundle_addrs; // for a bundle's address set
  OPENDDS_VECTOR(size_t) meta_submessage_bundle_sizes; // for allocating the bundle's buffer
  bundle_mapped_meta_submessages(adr_map, meta_submessage_bundles, meta_submessage_bundle_addrs, meta_submessage_bundle_sizes);

  // Reusable INFO_DST
  InfoDestinationSubmessage idst = {
    {INFO_DST, FLAG_E, INFO_DST_SZ},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}
  };

  // Allocate buffers, seralize, and send bundles
  RepoId prev_dst; // used to determine when we need to write a new info_dst
  for (size_t i = 0; i < meta_submessage_bundles.size(); ++i) {
    prev_dst = GUID_UNKNOWN;
    ACE_Message_Block mb_acknack(meta_submessage_bundle_sizes[i]); //FUTURE: allocators?
    Serializer ser(&mb_acknack, false, Serializer::ALIGN_CDR);
    for (MetaSubmessageIterVec::const_iterator it = meta_submessage_bundles[i].begin(); it != meta_submessage_bundles[i].end(); ++it) {
      MetaSubmessage& res = **it;
      RepoId dst = res.dst_guid_;
      dst.entityId = ENTITYID_UNKNOWN;
      if (dst != GUID_UNKNOWN && dst != prev_dst) {
        std::memcpy(&idst.guidPrefix, dst.guidPrefix, sizeof(idst.guidPrefix));
        ser << idst;
      }
      ser << res.sm_;
      prev_dst = dst;
    }
    send_strategy()->send_rtps_control(mb_acknack, meta_submessage_bundle_addrs[i]);
  }
}

void
RtpsUdpDataLink::send_heartbeat_replies() // from DR to DW
{
  using namespace OpenDDS::RTPS;

  MetaSubmessageVec meta_submessages;

  ACE_GUARD(ACE_Thread_Mutex, g, lock_);

  for (InterestingAckNackSetType::const_iterator pos = interesting_ack_nacks_.begin(),
         limit = interesting_ack_nacks_.end();
       pos != limit;
       ++pos) {

    SequenceNumber ack;
    LongSeq8 bitmap;
    bitmap.length(0);

    AckNackSubmessage acknack = {
      {ACKNACK,
       CORBA::Octet(FLAG_E | FLAG_F),
       0 /*length*/},
      pos->readerid.entityId,
      pos->writerid.entityId,
      { // SequenceNumberSet: acking bitmapBase - 1
        {ack.getHigh(), ack.getLow()},
        0 /* num_bits */, bitmap
      },
      {0 /* acknack count */}
    };

    MetaSubmessage meta_submessage(pos->readerid, pos->writerid);
    meta_submessage.sm_.acknack_sm(acknack);

    meta_submessages.push_back(meta_submessage);
  }
  interesting_ack_nacks_.clear();

  for (RtpsReaderMap::iterator rr = readers_.begin(); rr != readers_.end(); ++rr) {
    rr->second->gather_ack_nacks(meta_submessages);
  }

  g.release();

  send_bundled_submessages(meta_submessages);
}

void
RtpsUdpDataLink::RtpsReader::generate_nack_frags(NackFragSubmessageVec& nf,
                                                 WriterInfo& wi, const RepoId& pub_id)
{
  typedef OPENDDS_MAP(SequenceNumber, RTPS::FragmentNumber_t)::iterator iter_t;
  typedef RtpsUdpReceiveStrategy::FragmentInfo::value_type Frag_t;
  RtpsUdpReceiveStrategy::FragmentInfo frag_info;

  RtpsUdpDataLink_rch link = link_.lock();

  // Populate frag_info with two possible sources of NackFrags:
  // 1. sequence #s in the reception gaps that we have partially received
  OPENDDS_VECTOR(SequenceRange) missing = wi.recvd_.missing_sequence_ranges();
  for (size_t i = 0; i < missing.size(); ++i) {
    link->receive_strategy()->has_fragments(missing[i], pub_id, &frag_info);
  }
  // 1b. larger than the last received seq# but less than the heartbeat.lastSN
  if (!wi.recvd_.empty()) {
    const SequenceRange range(wi.recvd_.high(), wi.hb_range_.second);
    link->receive_strategy()->has_fragments(range, pub_id, &frag_info);
  }
  for (size_t i = 0; i < frag_info.size(); ++i) {
    // If we've received a HeartbeatFrag, we know the last (available) frag #
    const iter_t heartbeat_frag = wi.frags_.find(frag_info[i].first);
    if (heartbeat_frag != wi.frags_.end()) {
      extend_bitmap_range(frag_info[i].second, heartbeat_frag->second.value);
    }
  }

  // 2. sequence #s outside the recvd_ gaps for which we have a HeartbeatFrag
  const iter_t low = wi.frags_.lower_bound(wi.recvd_.cumulative_ack()),
              high = wi.frags_.upper_bound(wi.recvd_.last_ack()),
               end = wi.frags_.end();
  for (iter_t iter = wi.frags_.begin(); iter != end; ++iter) {
    if (iter == low) {
      // skip over the range covered by step #1 above
      if (high == end) {
        break;
      }
      iter = high;
    }

    const SequenceRange range(iter->first, iter->first);
    if (link->receive_strategy()->has_fragments(range, pub_id, &frag_info)) {
      extend_bitmap_range(frag_info.back().second, iter->second.value);
    } else {
      // it was not in the recv strategy, so the entire range is "missing"
      frag_info.push_back(Frag_t(iter->first, RTPS::FragmentNumberSet()));
      RTPS::FragmentNumberSet& fnSet = frag_info.back().second;
      fnSet.bitmapBase.value = 1;
      fnSet.numBits = std::min(CORBA::ULong(256), iter->second.value);
      fnSet.bitmap.length((fnSet.numBits + 31) / 32);
      for (CORBA::ULong i = 0; i < fnSet.bitmap.length(); ++i) {
        fnSet.bitmap[i] = 0xFFFFFFFF;
      }
    }
  }

  if (frag_info.empty()) {
    return;
  }

  const RTPS::NackFragSubmessage nackfrag_prototype = {
    {RTPS::NACK_FRAG, RTPS::FLAG_E, 0 /* length set below */},
    ENTITYID_UNKNOWN, // readerId will be filled-in by send_heartbeat_replies()
    ENTITYID_UNKNOWN, // writerId will be filled-in by send_heartbeat_replies()
    {0, 0}, // writerSN set below
    RTPS::FragmentNumberSet(), // fragmentNumberState set below
    {0} // count set below
  };

  for (size_t i = 0; i < frag_info.size(); ++i) {
    nf.push_back(nackfrag_prototype);
    RTPS::NackFragSubmessage& nackfrag = nf.back();
    nackfrag.writerSN.low = frag_info[i].first.getLow();
    nackfrag.writerSN.high = frag_info[i].first.getHigh();
    nackfrag.fragmentNumberState = frag_info[i].second;
    nackfrag.count.value = ++wi.nackfrag_count_;
  }
}

void
RtpsUdpDataLink::extend_bitmap_range(RTPS::FragmentNumberSet& fnSet,
                                     CORBA::ULong extent)
{
  if (extent < fnSet.bitmapBase.value) {
    return; // can't extend to some number under the base
  }
  // calculate the index to the extent to determine the new_num_bits
  const CORBA::ULong new_num_bits = std::min(CORBA::ULong(255),
                                             extent - fnSet.bitmapBase.value + 1),
                     len = (new_num_bits + 31) / 32;
  if (new_num_bits < fnSet.numBits) {
    return; // bitmap already extends past "extent"
  }
  fnSet.bitmap.length(len);
  // We are missing from one past old bitmap end to the new end
  DisjointSequence::fill_bitmap_range(fnSet.numBits + 1, new_num_bits,
                                      fnSet.bitmap.get_buffer(), len,
                                      fnSet.numBits);
}

void
RtpsUdpDataLink::received(const RTPS::HeartBeatFragSubmessage& hb_frag,
                          const GuidPrefix_t& src_prefix)
{
  datareader_dispatch(hb_frag, src_prefix, &RtpsReader::process_hb_frag_i);
}

bool
RtpsUdpDataLink::RtpsReader::process_hb_frag_i(const RTPS::HeartBeatFragSubmessage& hb_frag,
                                               const RepoId& src,
                                               MetaSubmessageVec&)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);

  WriterInfoMap::iterator wi = remote_writers_.find(src);
  if (wi == remote_writers_.end()) {
    // we may not be associated yet, even if the writer thinks we are
    return false;
  }

  if (hb_frag.count.value <= wi->second.hb_frag_recvd_count_) {
    return false;
  }

  wi->second.hb_frag_recvd_count_ = hb_frag.count.value;

  SequenceNumber seq;
  seq.setValue(hb_frag.writerSN.high, hb_frag.writerSN.low);

  // If seq is outside the heartbeat range or we haven't completely received
  // it yet, send a NackFrag along with the AckNack.  The heartbeat range needs
  // to be checked first because recvd_ contains the numbers below the
  // heartbeat range (so that we don't NACK those).
  if (seq < wi->second.hb_range_.first || seq > wi->second.hb_range_.second
      || !wi->second.recvd_.contains(seq)) {
    wi->second.frags_[seq] = hb_frag.lastFragmentNum;
    wi->second.ack_pending_ = true;
    return true; // timer will invoke send_heartbeat_replies()
  }
  return false;
}


// DataWriter's side of Reliability

void
RtpsUdpDataLink::received(const RTPS::AckNackSubmessage& acknack,
                          const GuidPrefix_t& src_prefix)
{
  // local side is DW
  RepoId local;
  std::memcpy(local.guidPrefix, local_prefix_, sizeof(GuidPrefix_t));
  local.entityId = acknack.writerId; // can't be ENTITYID_UNKNOWN

  RepoId remote;
  std::memcpy(remote.guidPrefix, src_prefix, sizeof(GuidPrefix_t));
  remote.entityId = acknack.readerId;

  const ACE_Time_Value now = ACE_OS::gettimeofday();
  OPENDDS_VECTOR(DiscoveryListener*) callbacks;

  {
    ACE_GUARD(ACE_Thread_Mutex, g, lock_);
    for (InterestingRemoteMapType::iterator pos = interesting_readers_.lower_bound(remote),
           limit = interesting_readers_.upper_bound(remote);
         pos != limit;
         ++pos) {
      pos->second.last_activity = now;
      // Ensure the acknack was for the writer.
      if (local == pos->second.localid) {
        if (pos->second.status == InterestingRemote::DOES_NOT_EXIST) {
          callbacks.push_back(pos->second.listener);
          pos->second.status = InterestingRemote::EXISTS;
        }
      }
    }
  }

  for (size_t i = 0; i < callbacks.size(); ++i) {
    callbacks[i]->reader_exists(remote, local);
  }

  datawriter_dispatch(acknack, src_prefix, &RtpsWriter::process_acknack);
}

void
RtpsUdpDataLink::RtpsWriter::gather_gaps_i(const RepoId& reader,
                                           const DisjointSequence& gaps,
                                           MetaSubmessageVec& meta_submessages)
{
  using namespace RTPS;
  // RTPS v2.1 8.3.7.4: the Gap sequence numbers are those in the range
  // [gapStart, gapListBase) and those in the SNSet.
  const SequenceNumber firstMissing = gaps.low(),
                       base = ++SequenceNumber(gaps.cumulative_ack());
  const SequenceNumber_t gapStart = {firstMissing.getHigh(),
                                     firstMissing.getLow()},
                         gapListBase = {base.getHigh(), base.getLow()};
  CORBA::ULong num_bits = 0;
  LongSeq8 bitmap;

  if (gaps.disjoint()) {
    bitmap.length(bitmap_num_longs(base, gaps.high()));
    gaps.to_bitmap(bitmap.get_buffer(), bitmap.length(), num_bits);
  } else {
    bitmap.length(1);
    bitmap[0] = 0;
    num_bits = 1;
  }

  MetaSubmessage meta_submessage(id_, reader);
  GapSubmessage gap = {
    {GAP, FLAG_E, 0 /*length determined later*/},
    reader.entityId,
    id_.entityId,
    gapStart,
    {gapListBase, num_bits, bitmap}
  };
  meta_submessage.sm_.gap_sm(gap);

  if (Transport_debug_level > 5) {
    const GuidConverter conv(id_);
    SequenceRange sr;
    sr.first.setValue(gap.gapStart.high, gap.gapStart.low);
    SequenceNumber srbase;
    srbase.setValue(gap.gapList.bitmapBase.high, gap.gapList.bitmapBase.low);
    sr.second = srbase.previous();
    ACE_DEBUG((LM_DEBUG, "(%P|%t) RtpsUdpDataLink::RtpsWriter::gather_gaps_i "
              "GAP with range [%q, %q] from %C\n",
              sr.first.getValue(), sr.second.getValue(),
              OPENDDS_STRING(conv).c_str()));
  }

  // For durable writers, change a non-directed Gap into multiple directed gaps.
  OPENDDS_VECTOR(RepoId) readers;
  if (durable_ && reader.entityId == ENTITYID_UNKNOWN) {
    if (Transport_debug_level > 5) {
      const GuidConverter local_conv(id_);
      ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::RtpsWriter::gather_gaps_i local %C "
                 "durable writer\n", OPENDDS_STRING(local_conv).c_str()));
    }
    for (ReaderInfoMap::iterator ri = remote_readers_.begin();
         ri != remote_readers_.end(); ++ri) {
      if (!ri->second.expecting_durable_data()) {
        readers.push_back(ri->first);
      } else if (Transport_debug_level > 5) {
        const GuidConverter remote_conv(ri->first);
        ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::RtpsWriter::gather_gaps_i reader "
                   "%C is expecting durable data, no GAP sent\n",
                   OPENDDS_STRING(remote_conv).c_str()));
      }
    }
    for (size_t i = 0; i < readers.size(); ++i) {
      std::memcpy(meta_submessage.dst_guid_.guidPrefix, readers[i].guidPrefix, sizeof(GuidPrefix_t));
      gap.readerId = readers[i].entityId;
      // potentially multiple meta_submessages, but all directed
      meta_submessages.push_back(meta_submessage);
    }
  } else {
    // single meta_submessage, possibly non-directed
    meta_submessages.push_back(meta_submessage);
  }
}

void
RtpsUdpDataLink::RtpsWriter::process_acknack(const RTPS::AckNackSubmessage& acknack,
                                             const RepoId& src,
                                             MetaSubmessageVec& meta_submessages)
{
  ACE_GUARD(ACE_Thread_Mutex, g, mutex_);

  RtpsUdpDataLink_rch link = link_.lock();

  if (!link) {
    return;
  }

  RepoId remote = src;

  bool first_ack = false;

  if (Transport_debug_level > 5) {
    GuidConverter local_conv(id_), remote_conv(remote);
    ACE_DEBUG((LM_DEBUG, "(%P|%t) RtpsUdpDataLink::received(ACKNACK) "
      "local %C remote %C\n", OPENDDS_STRING(local_conv).c_str(),
      OPENDDS_STRING(remote_conv).c_str()));
  }

  const ReaderInfoMap::iterator ri = remote_readers_.find(remote);
  if (ri == remote_readers_.end()) {
    VDBG((LM_WARNING, "(%P|%t) RtpsUdpDataLink::received(ACKNACK) "
      "WARNING ReaderInfo not found\n"));
    return;
  }

  if (acknack.count.value <= ri->second.acknack_recvd_count_) {
    VDBG((LM_WARNING, "(%P|%t) RtpsUdpDataLink::received(ACKNACK) "
      "WARNING Count indicates duplicate, dropping\n"));
    return;
  }

  ri->second.acknack_recvd_count_ = acknack.count.value;

  if (!ri->second.handshake_done_) {
    ri->second.handshake_done_ = true;
    first_ack = true;
  }

  // For first_acknowledged_by_reader
  SequenceNumber received_sn_base;
  received_sn_base.setValue(acknack.readerSNState.bitmapBase.high, acknack.readerSNState.bitmapBase.low);

  OPENDDS_MAP(SequenceNumber, TransportQueueElement*) pendingCallbacks;
  const bool is_final = acknack.smHeader.flags & RTPS::FLAG_F;

  if (!ri->second.durable_data_.empty()) {
    if (Transport_debug_level > 5) {
      const GuidConverter local_conv(id_), remote_conv(remote);
      ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::received(ACKNACK) "
                 "local %C has durable for remote %C\n",
                 OPENDDS_STRING(local_conv).c_str(),
                 OPENDDS_STRING(remote_conv).c_str()));
    }
    SequenceNumber ack;
    ack.setValue(acknack.readerSNState.bitmapBase.high,
                 acknack.readerSNState.bitmapBase.low);
    const SequenceNumber& dd_last = ri->second.durable_data_.rbegin()->first;
    if (Transport_debug_level > 5) {
      ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::received(ACKNACK) "
                 "check ack %q against last durable %q\n",
                 ack.getValue(), dd_last.getValue()));
    }
    if (ack > dd_last) {
      // Reader acknowledges durable data, we no longer need to store it
      ri->second.durable_data_.swap(pendingCallbacks);
      if (Transport_debug_level > 5) {
        ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::received(ACKNACK) "
                   "durable data acked\n"));
      }
    } else {
      DisjointSequence requests;
      if (!requests.insert(ack, acknack.readerSNState.numBits,
                           acknack.readerSNState.bitmap.get_buffer())
          && !is_final && ack == heartbeat_high(ri->second)) {
        // This is a non-final AckNack with no bits in the bitmap.
        // Attempt to reply to a request for the "base" value which
        // is neither Acked nor Nacked, only when it's the HB high.
        if (ri->second.durable_data_.count(ack)) requests.insert(ack);
      }
      // Attempt to reply to nacks for durable data
      bool sent_some = false;
      typedef OPENDDS_MAP(SequenceNumber, TransportQueueElement*)::iterator iter_t;
      iter_t it = ri->second.durable_data_.begin();
      const OPENDDS_VECTOR(SequenceRange) psr = requests.present_sequence_ranges();
      SequenceNumber lastSent = SequenceNumber::ZERO();
      if (!requests.empty()) {
        lastSent = requests.low().previous();
      }
      DisjointSequence gaps;
      for (size_t i = 0; i < psr.size(); ++i) {
        for (; it != ri->second.durable_data_.end()
             && it->first < psr[i].first; ++it) ; // empty for-loop
        for (; it != ri->second.durable_data_.end()
             && it->first <= psr[i].second; ++it) {
          if (Transport_debug_level > 5) {
            ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::received(ACKNACK) "
                       "durable resend %d\n", int(it->first.getValue())));
          }
          link->durability_resend(it->second);
          //FUTURE: combine multiple resends into one RTPS Message?
          sent_some = true;
          if (it->first > lastSent + 1) {
            gaps.insert(SequenceRange(lastSent + 1, it->first.previous()));
          }
          lastSent = it->first;
        }
        if (lastSent < psr[i].second && psr[i].second < dd_last) {
          gaps.insert(SequenceRange(lastSent + 1, psr[i].second));
          if (it != ri->second.durable_data_.end()) {
            gaps.insert(SequenceRange(psr[i].second, it->first.previous()));
          }
        }
      }
      if (!gaps.empty()) {
        if (Transport_debug_level > 5) {
          ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::received(ACKNACK) "
                     "sending durability gaps:\n"));
          gaps.dump();
        }
        gather_gaps_i(remote, gaps, meta_submessages);
      }
      if (sent_some) {
        return;
      }
      const SequenceNumber& dd_first = ri->second.durable_data_.begin()->first;
      if (!requests.empty() && requests.high() < dd_first) {
        // All nacks were below the start of the durable data.
          requests.insert(SequenceRange(requests.high(), dd_first.previous()));
        if (Transport_debug_level > 5) {
          ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::received(ACKNACK) "
                     "sending durability gaps for all requests:\n"));
          requests.dump();
        }
        gather_gaps_i(remote, requests, meta_submessages);
        return;
      }
      if (!requests.empty() && requests.low() < dd_first) {
        // Lowest nack was below the start of the durable data.
        for (size_t i = 0; i < psr.size(); ++i) {
          if (psr[i].first > dd_first) {
            break;
          }
          gaps.insert(SequenceRange(psr[i].first,
                                    std::min(psr[i].second, dd_first)));
        }
        if (Transport_debug_level > 5) {
          ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::received(ACKNACK) "
                     "sending durability gaps for some requests:\n"));
          gaps.dump();
        }
        gather_gaps_i(remote, gaps, meta_submessages);
        return;
      }
    }
  }
  SequenceNumber ack;
  ack.setValue(acknack.readerSNState.bitmapBase.high,
               acknack.readerSNState.bitmapBase.low);
  if (ack != SequenceNumber::SEQUENCENUMBER_UNKNOWN()
      && ack != SequenceNumber::ZERO()) {
    ri->second.cur_cumulative_ack_ = ack;
  }
  // If this ACKNACK was final, the DR doesn't expect a reply, and therefore
  // we don't need to do anything further.
  if (!is_final || bitmapNonEmpty(acknack.readerSNState)) {
    ri->second.requested_changes_.push_back(acknack.readerSNState);
  }

  SnToTqeMap to_deliver;
  acked_by_all_helper_i(to_deliver);

  if (!is_final) {
    link->nack_reply_.schedule(); // timer will invoke send_nack_replies()
  }
  typedef OPENDDS_MAP(SequenceNumber, TransportQueueElement*)::iterator iter_t;
  for (iter_t it = pendingCallbacks.begin();
       it != pendingCallbacks.end(); ++it) {
    it->second->data_delivered();
  }
  g.release();

  SnToTqeMap::iterator deliver_iter = to_deliver.begin();
  while (deliver_iter != to_deliver.end()) {
    deliver_iter->second->data_delivered();
    ++deliver_iter;
  }

  if (first_ack) {
    link->invoke_on_start_callbacks(id_, remote, true);
  }
}

void
RtpsUdpDataLink::received(const RTPS::NackFragSubmessage& nackfrag,
                          const GuidPrefix_t& src_prefix)
{
  datawriter_dispatch(nackfrag, src_prefix, &RtpsWriter::process_nackfrag);
}

void RtpsUdpDataLink::RtpsWriter::process_nackfrag(const RTPS::NackFragSubmessage& nackfrag,
                                                   const RepoId& src,
                                                   MetaSubmessageVec&)
{
  ACE_GUARD(ACE_Thread_Mutex, g, mutex_);

  RtpsUdpDataLink_rch link = link_.lock();

  if (!link) {
    return;
  }

  RepoId remote = src;

  if (Transport_debug_level > 5) {
    GuidConverter local_conv(id_), remote_conv(remote);
    ACE_DEBUG((LM_DEBUG, "(%P|%t) RtpsUdpDataLink::received(NACK_FRAG) "
      "local %C remote %C\n", OPENDDS_STRING(local_conv).c_str(),
      OPENDDS_STRING(remote_conv).c_str()));
  }

  const ReaderInfoMap::iterator ri = remote_readers_.find(remote);
  if (ri == remote_readers_.end()) {
    VDBG((LM_WARNING, "(%P|%t) RtpsUdpDataLink::received(NACK_FRAG) "
      "WARNING ReaderInfo not found\n"));
    return;
  }

  if (nackfrag.count.value <= ri->second.nackfrag_recvd_count_) {
    VDBG((LM_WARNING, "(%P|%t) RtpsUdpDataLink::received(NACK_FRAG) "
      "WARNING Count indicates duplicate, dropping\n"));
    return;
  }

  ri->second.nackfrag_recvd_count_ = nackfrag.count.value;

  SequenceNumber seq;
  seq.setValue(nackfrag.writerSN.high, nackfrag.writerSN.low);
  ri->second.requested_frags_[seq] = nackfrag.fragmentNumberState;

  link->nack_reply_.schedule(); // timer will invoke send_nack_replies()
}

void
RtpsUdpDataLink::RtpsWriter::send_and_gather_nack_replies(MetaSubmessageVec& meta_submessages)
{
  ACE_GUARD(ACE_Thread_Mutex, g, mutex_);

  RtpsUdpDataLink_rch link = link_.lock();

  if (!link) {
    return;
  }

  // consolidate requests from N readers
  AddrSet recipients;
  DisjointSequence requests;

  //track if any messages have been fully acked by all readers
  SequenceNumber all_readers_ack = SequenceNumber::MAX_VALUE;

#ifdef OPENDDS_SECURITY
  const EntityId_t& pvs_writer =
    RTPS::ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_WRITER;
  const bool is_pvs_writer =
    0 == std::memcmp(&pvs_writer, &id_.entityId, sizeof pvs_writer);
#endif

  typedef ReaderInfoMap::iterator ri_iter;
  const ri_iter end = remote_readers_.end();
  for (ri_iter ri = remote_readers_.begin(); ri != end; ++ri) {

    if (ri->second.cur_cumulative_ack_ < all_readers_ack) {
      all_readers_ack = ri->second.cur_cumulative_ack_;
    }

#ifdef OPENDDS_SECURITY
    if (is_pvs_writer && !ri->second.requested_changes_.empty()) {
      send_directed_nack_replies_i(ri->first, ri->second, meta_submessages);
      continue;
    }
#endif

    process_requested_changes_i(requests, ri->second);

    if (!ri->second.requested_changes_.empty()) {
      AddrSet addrs = link->get_addresses(id_, ri->first);
      if (!addrs.empty()) {
        recipients.insert(addrs.begin(), addrs.end());
        if (Transport_debug_level > 5) {
          const GuidConverter local_conv(id_), remote_conv(ri->first);
          ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::send_nack_replies "
                     "local %C remote %C requested resend\n",
                     OPENDDS_STRING(local_conv).c_str(),
                     OPENDDS_STRING(remote_conv).c_str()));
        }
      }
      ri->second.requested_changes_.clear();
    }
  }

  DisjointSequence gaps;
  if (!requests.empty()) {
    if (send_buff_.is_nil() || send_buff_->empty()) {
      gaps = requests;
    } else {
      OPENDDS_VECTOR(SequenceRange) ranges = requests.present_sequence_ranges();
      SingleSendBuffer& sb = *send_buff_;
      ACE_GUARD(TransportSendBuffer::LockType, guard, sb.strategy_lock());
      const RtpsUdpSendStrategy::OverrideToken ot =
        link->send_strategy()->override_destinations(recipients);
      for (size_t i = 0; i < ranges.size(); ++i) {
        if (Transport_debug_level > 5) {
          ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::send_nack_replies "
                     "resend data %d-%d\n", int(ranges[i].first.getValue()),
                     int(ranges[i].second.getValue())));
        }
        sb.resend_i(ranges[i], &gaps);
      }
    }
  }

  send_nackfrag_replies_i(gaps, recipients);

  if (!gaps.empty()) {
    if (Transport_debug_level > 5) {
      ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::send_nack_replies "
                 "GAPs:"));
      gaps.dump();
    }
    gather_gaps_i(GUID_UNKNOWN, gaps, meta_submessages);
  }
}

void
RtpsUdpDataLink::send_nack_replies()
{
  RtpsWriterMap writers;
  {
    ACE_GUARD(ACE_Thread_Mutex, g, lock_);
    writers = writers_;
  }

  MetaSubmessageVec meta_submessages;

  // Reply from local DW to remote DR: GAP or DATA
  using namespace OpenDDS::RTPS;
  typedef RtpsWriterMap::iterator rw_iter;
  for (rw_iter rw = writers.begin(); rw != writers.end(); ++rw) {
    rw->second->send_and_gather_nack_replies(meta_submessages);
  }

  send_bundled_submessages(meta_submessages);
}

void
RtpsUdpDataLink::RtpsWriter::send_nackfrag_replies_i(DisjointSequence& gaps,
                                                     AddrSet& gap_recipients)
{
  RtpsUdpDataLink_rch link = link_.lock();

  if (!link) {
    return;
  }

  typedef OPENDDS_MAP(SequenceNumber, DisjointSequence) FragmentInfo;
  OPENDDS_MAP(ACE_INET_Addr, FragmentInfo) requests;

  typedef ReaderInfoMap::iterator ri_iter;
  const ri_iter end = remote_readers_.end();
  for (ri_iter ri = remote_readers_.begin(); ri != end; ++ri) {

    if (ri->second.requested_frags_.empty()) {
      continue;
    }

    AddrSet remote_addrs = link->get_addresses(id_, ri->first);
    if (remote_addrs.empty()) {
      continue;
    }

    typedef OPENDDS_MAP(SequenceNumber, RTPS::FragmentNumberSet)::iterator rf_iter;
    const rf_iter rf_end = ri->second.requested_frags_.end();
    for (rf_iter rf = ri->second.requested_frags_.begin(); rf != rf_end; ++rf) {

      const SequenceNumber& seq = rf->first;
      if (send_buff_->contains(seq)) {
        for (AddrSet::const_iterator pos = remote_addrs.begin(), limit = remote_addrs.end();
             pos != limit; ++pos) {
          FragmentInfo& fi = requests[*pos];
          fi[seq].insert(rf->second.bitmapBase.value, rf->second.numBits,
                         rf->second.bitmap.get_buffer());
        }
      } else {
        gaps.insert(seq);
        gap_recipients.insert(remote_addrs.begin(), remote_addrs.end());
      }
    }
    ri->second.requested_frags_.clear();
  }

  typedef OPENDDS_MAP(ACE_INET_Addr, FragmentInfo)::iterator req_iter;
  for (req_iter req = requests.begin(); req != requests.end(); ++req) {
    const FragmentInfo& fi = req->second;

    ACE_GUARD(TransportSendBuffer::LockType, guard,
      send_buff_->strategy_lock());
    const RtpsUdpSendStrategy::OverrideToken ot =
      link->send_strategy()->override_destinations(req->first);

    for (FragmentInfo::const_iterator sn_iter = fi.begin();
         sn_iter != fi.end(); ++sn_iter) {
      const SequenceNumber& seq = sn_iter->first;
      send_buff_->resend_fragments_i(seq, sn_iter->second);
    }
  }
}

void
RtpsUdpDataLink::RtpsWriter::process_requested_changes_i(DisjointSequence& requests,
                                                         const ReaderInfo& reader)
{
  for (size_t i = 0; i < reader.requested_changes_.size(); ++i) {
    const RTPS::SequenceNumberSet& sn_state = reader.requested_changes_[i];
    SequenceNumber base;
    base.setValue(sn_state.bitmapBase.high, sn_state.bitmapBase.low);
    if (sn_state.numBits == 1 && !(sn_state.bitmap[0] & 1)
        && base == heartbeat_high(reader)) {
      // Since there is an entry in requested_changes_, the DR must have
      // sent a non-final AckNack.  If the base value is the high end of
      // the heartbeat range, treat it as a request for that seq#.
      if (!send_buff_.is_nil() && send_buff_->contains(base)) {
        requests.insert(base);
      }
    } else {
      requests.insert(base, sn_state.numBits, sn_state.bitmap.get_buffer());
    }
  }
}

void
RtpsUdpDataLink::RtpsWriter::send_directed_nack_replies_i(const RepoId& readerId,
                                                          ReaderInfo& reader,
                                                          MetaSubmessageVec& meta_submessages)
{
  RtpsUdpDataLink_rch link = link_.lock();

  if (!link) {
    return;
  }

  AddrSet addrs = link->get_addresses(id_, readerId);
  if (addrs.empty()) {
    return;
  }

  DisjointSequence requests;
  process_requested_changes_i(requests, reader);
  reader.requested_changes_.clear();

  DisjointSequence gaps;

  if (!requests.empty()) {
    if (send_buff_.is_nil() || send_buff_->empty()) {
      gaps = requests;
    } else {
      OPENDDS_VECTOR(SequenceRange) ranges = requests.present_sequence_ranges();
      SingleSendBuffer& sb = *send_buff_;
      ACE_GUARD(TransportSendBuffer::LockType, guard, sb.strategy_lock());
      const RtpsUdpSendStrategy::OverrideToken ot =
        link->send_strategy()->override_destinations(addrs);
      for (size_t i = 0; i < ranges.size(); ++i) {
        if (Transport_debug_level > 5) {
          ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::send_directed_nack_replies "
                     "resend data %d-%d\n", int(ranges[i].first.getValue()),
                     int(ranges[i].second.getValue())));
        }
        sb.resend_i(ranges[i], &gaps, readerId);
      }
    }
  }

  if (gaps.empty()) {
    return;
  }
  if (Transport_debug_level > 5) {
    ACE_DEBUG((LM_DEBUG, "RtpsUdpDataLink::send_directed_nack_replies GAPs: "));
    gaps.dump();
  }
  gather_gaps_i(readerId, gaps, meta_submessages);
}

void
RtpsUdpDataLink::RtpsWriter::process_acked_by_all()
{
  ACE_GUARD(ACE_Thread_Mutex, g, mutex_);

  SnToTqeMap to_deliver;
  acked_by_all_helper_i(to_deliver);

  SnToTqeMap::iterator deliver_iter = to_deliver.begin();
  while (deliver_iter != to_deliver.end()) {
    deliver_iter->second->data_delivered();
    ++deliver_iter;
  }
}

void
RtpsUdpDataLink::RtpsWriter::acked_by_all_helper_i(SnToTqeMap& to_deliver)
{
  using namespace OpenDDS::RTPS;
  typedef OPENDDS_MULTIMAP(SequenceNumber, TransportQueueElement*)::iterator iter_t;
  OPENDDS_VECTOR(RepoId) to_check;

  RtpsUdpDataLink_rch link = link_.lock();

  if (!link) {
    return;
  }

  if (!elems_not_acked_.empty()) {

    //start with the max sequence number writer knows about and decrease
    //by what the min over all readers is
    SequenceNumber all_readers_ack = SequenceNumber::MAX_VALUE;

    typedef ReaderInfoMap::iterator ri_iter;
    const ri_iter end = remote_readers_.end();
    for (ri_iter ri = remote_readers_.begin(); ri != end; ++ri) {
      if (ri->second.cur_cumulative_ack_ < all_readers_ack) {
        all_readers_ack = ri->second.cur_cumulative_ack_;
      }
    }
    if (all_readers_ack == SequenceNumber::MAX_VALUE) {
      return;
    }

    OPENDDS_SET(SequenceNumber) sns_to_release;
    iter_t it = elems_not_acked_.begin();
    while (it != elems_not_acked_.end()) {
      if (it->first < all_readers_ack) {
        to_deliver.insert(RtpsWriter::SnToTqeMap::value_type(it->first, it->second));
        sns_to_release.insert(it->first);
        iter_t last = it;
        ++it;
        elems_not_acked_.erase(last);
      } else {
        break;
      }
    }
    OPENDDS_SET(SequenceNumber)::iterator sns_it = sns_to_release.begin();
    while (sns_it != sns_to_release.end()) {
      send_buff_->release_acked(*sns_it);
      ++sns_it;
    }
  }
}

void
RtpsUdpDataLink::durability_resend(TransportQueueElement* element)
{
  ACE_Message_Block* msg = const_cast<ACE_Message_Block*>(element->msg());
  AddrSet addrs = get_addresses(element->publication_id(), element->subscription_id());
  if (addrs.empty()) {
    const GuidConverter conv(element->subscription_id());
    ACE_ERROR((LM_ERROR,
               "(%P|%t) RtpsUdpDataLink::durability_resend() - "
               "no locator for remote %C\n", OPENDDS_STRING(conv).c_str()));
  } else {
    send_strategy()->send_rtps_control(*msg, addrs);
  }
}

void
RtpsUdpDataLink::send_heartbeats()
{
  OPENDDS_VECTOR(CallbackType) readerDoesNotExistCallbacks;
  OPENDDS_VECTOR(TransportQueueElement*) pendingCallbacks;

  const ACE_Time_Value now = ACE_OS::gettimeofday();
  RtpsWriterMap writers;

  typedef OPENDDS_MAP_CMP(RepoId, RepoIdSet, GUID_tKeyLessThan) WtaMap;
  WtaMap writers_to_advertise;

  {
    ACE_GUARD(ACE_Thread_Mutex, g, lock_);

    RtpsUdpInst& cfg = config();

    const ACE_Time_Value tv = now - 10 * cfg.heartbeat_period_;
    const ACE_Time_Value tv3 = now - 3 * cfg.heartbeat_period_;

    for (InterestingRemoteMapType::iterator pos = interesting_readers_.begin(),
           limit = interesting_readers_.end();
         pos != limit;
         ++pos) {
      if (pos->second.status == InterestingRemote::DOES_NOT_EXIST ||
          (pos->second.status == InterestingRemote::EXISTS && pos->second.last_activity < tv3)) {
          writers_to_advertise[pos->second.localid].insert(pos->first);
      }
      if (pos->second.status == InterestingRemote::EXISTS && pos->second.last_activity < tv) {
        CallbackType callback(pos->first, pos->second);
        readerDoesNotExistCallbacks.push_back(callback);
        pos->second.status = InterestingRemote::DOES_NOT_EXIST;
      }
    }

    if (writers_.empty() && interesting_readers_.empty()) {
      heartbeat_->disable();
    }

    writers = writers_;
  }

  using namespace OpenDDS::RTPS;

  MetaSubmessageVec meta_submessages;

  using namespace OpenDDS::RTPS;
  typedef RtpsWriterMap::iterator rw_iter;
  for (rw_iter rw = writers.begin(); rw != writers.end(); ++rw) {
    WtaMap::iterator it = writers_to_advertise.find(rw->first);
    if (it == writers_to_advertise.end()) {
      rw->second->gather_heartbeats(pendingCallbacks, RepoIdSet(), true, meta_submessages);
    } else {
      if (rw->second->gather_heartbeats(pendingCallbacks, it->second, false, meta_submessages)) {
        writers_to_advertise.erase(it);
      }
    }
  }

  for (WtaMap::const_iterator pos = writers_to_advertise.begin(),
         limit = writers_to_advertise.end();
       pos != limit;
       ++pos) {
    const SequenceNumber SN = 1, lastSN = SequenceNumber::ZERO();
    const HeartBeatSubmessage hb = {
      {HEARTBEAT, FLAG_E, HEARTBEAT_SZ},
      ENTITYID_UNKNOWN, // any matched reader may be interested in this
      pos->first.entityId,
      {SN.getHigh(), SN.getLow()},
      {lastSN.getHigh(), lastSN.getLow()},
      {++heartbeat_counts_[pos->first]}
    };

    MetaSubmessage meta_submessage(pos->first, GUID_UNKNOWN, pos->second);
    meta_submessage.sm_.heartbeat_sm(hb);

    meta_submessages.push_back(meta_submessage);
  }

  send_bundled_submessages(meta_submessages);

  for (OPENDDS_VECTOR(CallbackType)::iterator iter = readerDoesNotExistCallbacks.begin();
      iter != readerDoesNotExistCallbacks.end(); ++iter) {
    const InterestingRemote& remote = iter->second;
    remote.listener->reader_does_not_exist(iter->first, remote.localid);
  }

  for (size_t i = 0; i < pendingCallbacks.size(); ++i) {
    pendingCallbacks[i]->data_dropped();
  }
}

bool
RtpsUdpDataLink::RtpsWriter::gather_heartbeats(OPENDDS_VECTOR(TransportQueueElement*)& pendingCallbacks,
                                               const RepoIdSet& additional_guids,
                                               bool allow_final,
                                               MetaSubmessageVec& meta_submessages)
{
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, mutex_, false);

  RtpsUdpDataLink_rch link = link_.lock();
  if (!link) {
    return false;
  }

  const bool has_data = !send_buff_.is_nil()
                        && !send_buff_->empty();
  bool is_final = allow_final, has_durable_data = false;
  SequenceNumber durable_max = SequenceNumber::ZERO();

  MetaSubmessage meta_submessage(id_, GUID_UNKNOWN);
  meta_submessage.to_guids_ = additional_guids;

  const ACE_Time_Value now = ACE_OS::gettimeofday();
  RtpsUdpInst& cfg = link->config();

  // Directed, non-final pre-association heartbeats
  RepoIdSet pre_assoc_hb_guids;

  typedef ReaderInfoMap::iterator ri_iter;
  const ri_iter end = remote_readers_.end();
  for (ri_iter ri = remote_readers_.begin(); ri != end; ++ri) {
    if (has_data) {
      meta_submessage.to_guids_.insert(ri->first);
    } else if (!ri->second.handshake_done_) {
      pre_assoc_hb_guids.insert(ri->first);
    }
    if (!ri->second.durable_data_.empty()) {
      const ACE_Time_Value expiration =
        ri->second.durable_timestamp_ + cfg.durable_data_timeout_;
      if (now > expiration) {
        typedef OPENDDS_MAP(SequenceNumber, TransportQueueElement*)::iterator
          dd_iter;
        for (dd_iter it = ri->second.durable_data_.begin();
             it != ri->second.durable_data_.end(); ++it) {
          pendingCallbacks.push_back(it->second);
        }
        ri->second.durable_data_.clear();
        if (Transport_debug_level > 3) {
          const GuidConverter gw(id_), gr(ri->first);
          VDBG_LVL((LM_INFO, "(%P|%t) RtpsUdpDataLink::send_heartbeats - "
            "removed expired durable data for %C -> %C\n",
            OPENDDS_STRING(gw).c_str(), OPENDDS_STRING(gr).c_str()), 3);
        }
      } else {
        has_durable_data = true;
        if (ri->second.durable_data_.rbegin()->first > durable_max) {
          durable_max = ri->second.durable_data_.rbegin()->first;
        }
        meta_submessage.to_guids_.insert(ri->first);
      }
    }
  }

  if (!elems_not_acked_.empty()) {
    is_final = false;
  }

  const SequenceNumber firstSN = (durable_ || !has_data) ? 1 : send_buff_->low(),
    lastSN = std::max(durable_max, has_data ? send_buff_->high() : SequenceNumber::ZERO());
  using namespace OpenDDS::RTPS;

  const HeartBeatSubmessage hb = {
    {HEARTBEAT,
     CORBA::Octet(FLAG_E | (is_final ? FLAG_F : 0)),
     HEARTBEAT_SZ},
    ENTITYID_UNKNOWN, // any matched reader may be interested in this
    id_.entityId,
    {firstSN.getHigh(), firstSN.getLow()},
    {lastSN.getHigh(), lastSN.getLow()},
    {++heartbeat_count_}
  };
  meta_submessage.sm_.heartbeat_sm(hb);

  // Directed, non-final pre-association heartbeats
  MetaSubmessage pre_assoc_hb = meta_submessage;
  pre_assoc_hb.to_guids_.clear();
  pre_assoc_hb.sm_.heartbeat_sm().smHeader.flags &= ~(FLAG_F);
  for (RepoIdSet::const_iterator it = pre_assoc_hb_guids.begin(); it != pre_assoc_hb_guids.end(); ++it) {
    pre_assoc_hb.dst_guid_ = (*it);
    pre_assoc_hb.sm_.heartbeat_sm().readerId = it->entityId;
    meta_submessages.push_back(pre_assoc_hb);
  }

  if (is_final && !has_data && !has_durable_data) {
    return true;
  }

#ifdef OPENDDS_SECURITY
  const EntityId_t& volatile_writer =
    RTPS::ENTITYID_P2P_BUILTIN_PARTICIPANT_VOLATILE_SECURE_WRITER;
  if (std::memcmp(&id_.entityId, &volatile_writer, sizeof(EntityId_t)) == 0) {
    RepoIdSet guids = meta_submessage.to_guids_;
    meta_submessage.to_guids_.clear();
    for (RepoIdSet::const_iterator it = guids.begin(); it != guids.end(); ++it) {
      meta_submessage.dst_guid_ = (*it);
      meta_submessage.sm_.heartbeat_sm().readerId = it->entityId;
      meta_submessages.push_back(meta_submessage);
    }
  } else {
    meta_submessages.push_back(meta_submessage);
  }
#else
  meta_submessages.push_back(meta_submessage);
#endif
  return true;
}

void
RtpsUdpDataLink::check_heartbeats()
{
  OPENDDS_VECTOR(CallbackType) writerDoesNotExistCallbacks;

  // Have any interesting writers timed out?
  const ACE_Time_Value tv = ACE_OS::gettimeofday() - 10 * config().heartbeat_period_;
  {
    ACE_GUARD(ACE_Thread_Mutex, g, lock_);

    for (InterestingRemoteMapType::iterator pos = interesting_writers_.begin(), limit = interesting_writers_.end();
         pos != limit;
         ++pos) {
      if (pos->second.status == InterestingRemote::EXISTS && pos->second.last_activity < tv) {
        CallbackType callback(pos->first, pos->second);
        writerDoesNotExistCallbacks.push_back(callback);
        pos->second.status = InterestingRemote::DOES_NOT_EXIST;
      }
    }
  }

  OPENDDS_VECTOR(CallbackType)::iterator iter;
  for (iter = writerDoesNotExistCallbacks.begin(); iter != writerDoesNotExistCallbacks.end(); ++iter) {
    const RepoId& rid = iter->first;
    const InterestingRemote& remote = iter->second;
    remote.listener->writer_does_not_exist(rid, remote.localid);
  }
}

void
RtpsUdpDataLink::send_relay_beacon()
{
  const bool no_relay = config().rtps_relay_address() == ACE_INET_Addr();
  {
    ACE_GUARD(ACE_Thread_Mutex, g, lock_);
    if (no_relay && readers_.empty()) {
      relay_beacon_->disable();
      return;
    }
  }

  // Create a message with a few bytes of data for the beacon
  ACE_Message_Block mb(reinterpret_cast<const char*>(OpenDDS::RTPS::BEACON_MESSAGE), OpenDDS::RTPS::BEACON_MESSAGE_LENGTH);
  mb.wr_ptr(OpenDDS::RTPS::BEACON_MESSAGE_LENGTH);
  send_strategy()->send_rtps_control(mb, config().rtps_relay_address());
}

void
RtpsUdpDataLink::send_heartbeats_manual_i(const TransportSendControlElement* tsce, MetaSubmessageVec& meta_submessages)
{
  using namespace OpenDDS::RTPS;

  const RepoId pub_id = tsce->publication_id();

  SequenceNumber firstSN, lastSN;
  CORBA::Long counter;

  firstSN = 1;
  lastSN = tsce->sequence();
  counter = ++best_effort_heartbeat_count_;

  const HeartBeatSubmessage hb = {
    {HEARTBEAT,
     CORBA::Octet(FLAG_E | FLAG_F | FLAG_L),
     HEARTBEAT_SZ},
    ENTITYID_UNKNOWN, // any matched reader may be interested in this
    pub_id.entityId,
    {firstSN.getHigh(), firstSN.getLow()},
    {lastSN.getHigh(), lastSN.getLow()},
    {counter}
  };

  MetaSubmessage meta_submessage(pub_id, GUID_UNKNOWN);
  meta_submessage.sm_.heartbeat_sm(hb);

  meta_submessages.push_back(meta_submessage);
}

void
RtpsUdpDataLink::RtpsWriter::send_heartbeats_manual_i(MetaSubmessageVec& meta_submessages)
{
  using namespace OpenDDS::RTPS;

  RtpsUdpDataLink_rch link = link_.lock();
  if (!link) {
    return;
  }

  SequenceNumber firstSN, lastSN;
  CORBA::Long counter;

  const bool has_data = !send_buff_.is_nil() && !send_buff_->empty();
  SequenceNumber durable_max;
  const ACE_Time_Value now = ACE_OS::gettimeofday();
  for (ReaderInfoMap::const_iterator ri = remote_readers_.begin(), end = remote_readers_.end();
       ri != end;
       ++ri) {
    if (!ri->second.durable_data_.empty()) {
      const ACE_Time_Value expiration = ri->second.durable_timestamp_ + link->config().durable_data_timeout_;
      if (now <= expiration &&
          ri->second.durable_data_.rbegin()->first > durable_max) {
        durable_max = ri->second.durable_data_.rbegin()->first;
      }
    }
  }

  firstSN = (durable_ || !has_data) ? 1 : send_buff_->low();
  lastSN = std::max(durable_max, has_data ? send_buff_->high() : 1);
  counter = ++heartbeat_count_;

  const HeartBeatSubmessage hb = {
    {HEARTBEAT,
     CORBA::Octet(FLAG_E | FLAG_F | FLAG_L),
     HEARTBEAT_SZ},
    ENTITYID_UNKNOWN, // any matched reader may be interested in this
    id_.entityId,
    {firstSN.getHigh(), firstSN.getLow()},
    {lastSN.getHigh(), lastSN.getLow()},
    {counter}
  };

  MetaSubmessage meta_submessage(id_, GUID_UNKNOWN);
  meta_submessage.sm_.heartbeat_sm(hb);

  meta_submessages.push_back(meta_submessage);
}

#ifdef OPENDDS_SECURITY
void
RtpsUdpDataLink::populate_security_handles(const RepoId& local_id,
                                           const RepoId& remote_id,
                                           const unsigned char* buffer,
                                           unsigned int buffer_size)
{
  using DDS::Security::ParticipantCryptoHandle;
  using DDS::Security::DatawriterCryptoHandle;
  using DDS::Security::DatareaderCryptoHandle;

  ACE_Data_Block db(buffer_size, ACE_Message_Block::MB_DATA,
    reinterpret_cast<const char*>(buffer),
    0 /*alloc*/, 0 /*lock*/, ACE_Message_Block::DONT_DELETE, 0 /*db_alloc*/);
  ACE_Message_Block mb(&db, ACE_Message_Block::DONT_DELETE, 0 /*mb_alloc*/);
  mb.wr_ptr(mb.space());
  DCPS::Serializer ser(&mb, ACE_CDR_BYTE_ORDER, DCPS::Serializer::ALIGN_CDR);

  const bool local_is_writer = GuidConverter(local_id).isWriter();
  const RepoId& writer_id = local_is_writer ? local_id : remote_id;
  const RepoId& reader_id = local_is_writer ? remote_id : local_id;

  ACE_GUARD(ACE_Thread_Mutex, g, ch_lock_);

  while (mb.length()) {
    DDS::BinaryProperty_t prop;
    if (!(ser >> prop)) {
      ACE_ERROR((LM_ERROR, "(%P|%t) RtpsUdpDataLink::populate_security_handles()"
                 " - failed to deserialize BinaryProperty_t\n"));
      return;
    }

    if (std::strcmp(prop.name.in(), RTPS::BLOB_PROP_PART_CRYPTO_HANDLE) == 0
        && prop.value.length() >= sizeof(ParticipantCryptoHandle)) {
      unsigned int handle = 0;
      for (unsigned int i = 0; i < prop.value.length(); ++i) {
        handle = handle << 8 | prop.value[i];
      }

      RepoId remote_participant;
      RTPS::assign(remote_participant.guidPrefix, remote_id.guidPrefix);
      remote_participant.entityId = ENTITYID_PARTICIPANT;
      peer_crypto_handles_[remote_participant] = handle;
      if (security_debug.bookkeeping) {
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) {bookkeeping} RtpsUdpDataLink::populate_security_handles() ")
                   ACE_TEXT("RPCH %C = %d\n"),
                   OPENDDS_STRING(GuidConverter(remote_participant)).c_str(), handle));
      }
    }

    else if (std::strcmp(prop.name.in(), RTPS::BLOB_PROP_DW_CRYPTO_HANDLE) == 0
             && prop.value.length() >= sizeof(DatawriterCryptoHandle)) {
      unsigned int handle = 0;
      for (unsigned int i = 0; i < prop.value.length(); ++i) {
        handle = handle << 8 | prop.value[i];
      }
      peer_crypto_handles_[writer_id] = handle;
      if (security_debug.bookkeeping) {
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) {bookkeeping} RtpsUdpDataLink::populate_security_handles() ")
                   ACE_TEXT("DWCH %C = %d\n"),
                   OPENDDS_STRING(GuidConverter(writer_id)).c_str(), handle));
      }
    }

    else if (std::strcmp(prop.name.in(), RTPS::BLOB_PROP_DR_CRYPTO_HANDLE) == 0
             && prop.value.length() >= sizeof(DatareaderCryptoHandle)) {
      unsigned int handle = 0;
      for (unsigned int i = 0; i < prop.value.length(); ++i) {
        handle = handle << 8 | prop.value[i];
      }
      peer_crypto_handles_[reader_id] = handle;
      if (security_debug.bookkeeping) {
        ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) {bookkeeping} RtpsUdpDataLink::populate_security_handles() ")
                   ACE_TEXT("DRCH %C = %d\n"),
                   std::string(GuidConverter(reader_id)).c_str(), handle));
      }
    }

  }
}
#endif

RtpsUdpDataLink::ReaderInfo::~ReaderInfo()
{
  expire_durable_data();
}

void
RtpsUdpDataLink::ReaderInfo::expire_durable_data()
{
  typedef OPENDDS_MAP(SequenceNumber, TransportQueueElement*)::iterator iter_t;
  for (iter_t it = durable_data_.begin(); it != durable_data_.end(); ++it) {
    it->second->data_dropped();
  }
}

bool
RtpsUdpDataLink::ReaderInfo::expecting_durable_data() const
{
  return durable_ &&
    (durable_timestamp_ == ACE_Time_Value::zero // DW hasn't resent yet
     || !durable_data_.empty());                // DW resent, not sent to reader
}

RtpsUdpDataLink::RtpsWriter::~RtpsWriter()
{
  if (!to_deliver_.empty()) {
    ACE_DEBUG((LM_WARNING, "(%P|%t) WARNING: RtpsWriter::~RtpsWriter - deleting with %d elements left to deliver\n", to_deliver_.size()));
  }
  if (!elems_not_acked_.empty()) {
    ACE_DEBUG((LM_WARNING, "(%P|%t) WARNING: RtpsWriter::~RtpsWriter - deleting with %d elements left not fully acknowledged\n", elems_not_acked_.size()));
  }
}

SequenceNumber
RtpsUdpDataLink::RtpsWriter::heartbeat_high(const ReaderInfo& ri) const
{
  const SequenceNumber durable_max =
    ri.durable_data_.empty() ? 0 : ri.durable_data_.rbegin()->first;
  const SequenceNumber data_max =
    send_buff_.is_nil() ? 0 : (send_buff_->empty() ? 0 : send_buff_->high());
  return std::max(durable_max, data_max);
}

void
RtpsUdpDataLink::RtpsWriter::add_elem_awaiting_ack(TransportQueueElement* element)
{
  elems_not_acked_.insert(SnToTqeMap::value_type(element->sequence(), element));
}


// Implementing TimedDelay and HeartBeat nested classes (for ACE timers)

void
RtpsUdpDataLink::TimedDelay::schedule()
{
  if (!scheduled_) {
    const long timer = outer_->get_reactor()->schedule_timer(this, 0, timeout_);

    if (timer == -1) {
      ACE_ERROR((LM_ERROR, "(%P|%t) RtpsUdpDataLink::TimedDelay::schedule "
        "failed to schedule timer %p\n", ACE_TEXT("")));
    } else {
      scheduled_ = true;
    }
  }
}

void
RtpsUdpDataLink::TimedDelay::cancel()
{
  if (scheduled_) {
    outer_->get_reactor()->cancel_timer(this);
    scheduled_ = false;
  }
}

void
RtpsUdpDataLink::HeartBeat::enable(bool reenable)
{
  if (!enabled_) {
    const ACE_Time_Value& per = outer_->config().heartbeat_period_;
    const long timer =
      outer_->get_reactor()->schedule_timer(this, 0, ACE_Time_Value::zero, per);

    if (timer == -1) {
      ACE_ERROR((LM_ERROR, "(%P|%t) RtpsUdpDataLink::HeartBeat::enable"
        " failed to schedule timer %p\n", ACE_TEXT("")));
    } else {
      enabled_ = true;
    }
  } else if (reenable) {
    disable();
    enable(false);
  }
}

void
RtpsUdpDataLink::HeartBeat::disable()
{
  if (enabled_) {
    outer_->get_reactor()->cancel_timer(this);
    enabled_ = false;
  }
}

void
RtpsUdpDataLink::send_final_acks(const RepoId& readerid)
{
  ACE_GUARD(ACE_Thread_Mutex, g, lock_);
  RtpsReaderMap::iterator rr = readers_.find(readerid);
  MetaSubmessageVec meta_submessages;
  if (rr != readers_.end()) {
    rr->second->gather_ack_nacks(meta_submessages, true);
  }
  g.release();
  send_bundled_submessages(meta_submessages);
}


int
RtpsUdpDataLink::HeldDataDeliveryHandler::handle_exception(ACE_HANDLE /* fd */)
{
  OPENDDS_ASSERT(link_->reactor_task_->get_reactor_owner() == ACE_Thread::self());

  HeldData::iterator itr;
  for (itr = held_data_.begin(); itr != held_data_.end(); ++itr) {
    link_->data_received(itr->first, itr->second);
  }
  held_data_.clear();

  return 0;
}

void RtpsUdpDataLink::HeldDataDeliveryHandler::notify_delivery(const RepoId& readerId, WriterInfo& info)
{
  OPENDDS_ASSERT(link_->reactor_task_->get_reactor_owner() == ACE_Thread::self());

  const SequenceNumber ca = info.recvd_.cumulative_ack();
  typedef OPENDDS_MAP(SequenceNumber, ReceivedDataSample)::iterator iter;
  const iter end = info.held_.upper_bound(ca);

  for (iter it = info.held_.begin(); it != end; /*increment in loop body*/) {
    if (Transport_debug_level > 5) {
      GuidConverter reader(readerId);
      ACE_DEBUG((LM_DEBUG, ACE_TEXT("(%P|%t) RtpsUdpDataLink::HeldDataDeliveryHandler::notify_delivery -")
                           ACE_TEXT(" deliver sequence: %q to %C\n"),
                           it->second.header_.sequence_.getValue(),
                           OPENDDS_STRING(reader).c_str()));
    }
    // The head_data_ is not protected by a mutex because it is always accessed from the reactor task thread.
    held_data_.push_back(HeldDataEntry(it->second, readerId));
    info.held_.erase(it++);
  }
  link_->reactor_task_->get_reactor()->notify(this);
}

ACE_Event_Handler::Reference_Count
RtpsUdpDataLink::HeldDataDeliveryHandler::add_reference()
{
  return link_->add_reference();
}

ACE_Event_Handler::Reference_Count
RtpsUdpDataLink::HeldDataDeliveryHandler::remove_reference()
{
  return link_->remove_reference();
}

RtpsUdpSendStrategy*
RtpsUdpDataLink::send_strategy()
{
  return static_cast<RtpsUdpSendStrategy*>(send_strategy_.in());
}

RtpsUdpReceiveStrategy*
RtpsUdpDataLink::receive_strategy()
{
  return static_cast<RtpsUdpReceiveStrategy*>(receive_strategy_.in());
}

RtpsUdpDataLink::AddrSet
RtpsUdpDataLink::get_addresses(const RepoId& local, const RepoId& remote) const {
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, lock_, AddrSet());
  return get_addresses_i(local, remote);
}

RtpsUdpDataLink::AddrSet
RtpsUdpDataLink::get_addresses(const RepoId& local) const {
  ACE_GUARD_RETURN(ACE_Thread_Mutex, g, lock_, AddrSet());
  return get_addresses_i(local);
}

RtpsUdpDataLink::AddrSet
RtpsUdpDataLink::get_addresses_i(const RepoId& local, const RepoId& remote) const {
  AddrSet retval;

  accumulate_addresses(local, remote, retval);

  return retval;
}

RtpsUdpDataLink::AddrSet
RtpsUdpDataLink::get_addresses_i(const RepoId& local) const {
  AddrSet retval;

  const GUIDSeq_var peers = peer_ids(local);
  if (peers.ptr()) {
    for (CORBA::ULong i = 0; i < peers->length(); ++i) {
      accumulate_addresses(local, peers[i], retval);
    }
  }

  return retval;
}

void
RtpsUdpDataLink::accumulate_addresses(const RepoId& local, const RepoId& remote,
                                                     AddrSet& addresses) const {
  ACE_UNUSED_ARG(local);
  OPENDDS_ASSERT(local != GUID_UNKNOWN);
  OPENDDS_ASSERT(remote != GUID_UNKNOWN);

  ACE_INET_Addr normal_addr;
  ACE_INET_Addr ice_addr;
  static const ACE_INET_Addr NO_ADDR;

  typedef OPENDDS_MAP_CMP(RepoId, RemoteInfo, GUID_tKeyLessThan)::const_iterator iter_t;
  iter_t pos = locators_.find(remote);
  if (pos != locators_.end()) {
    normal_addr = pos->second.addr_;
  } else {
    const GuidConverter conv(remote);
    if (conv.isReader()) {
      InterestingRemoteMapType::const_iterator ipos = interesting_readers_.find(remote);
      if (ipos != interesting_readers_.end()) {
        normal_addr = ipos->second.address;
      }
    } else if (conv.isWriter()) {
      InterestingRemoteMapType::const_iterator ipos = interesting_writers_.find(remote);
      if (ipos != interesting_writers_.end()) {
        normal_addr = ipos->second.address;
      }
    }
  }

#ifdef OPENDDS_SECURITY
  ICE::Endpoint* endpoint = get_ice_endpoint();
  if (endpoint) {
    ice_addr = ICE::Agent::instance()->get_address(endpoint, local, remote);
  }
#endif

  if (ice_addr == NO_ADDR) {
    if (normal_addr != NO_ADDR) {
      addresses.insert(normal_addr);
    }
    ACE_INET_Addr relay_addr = config().rtps_relay_address();
    if (relay_addr != NO_ADDR) {
      addresses.insert(relay_addr);
    }
    return;
  }

  if (ice_addr != normal_addr) {
    addresses.insert(ice_addr);
    return;
  }

  if (normal_addr != NO_ADDR) {
    addresses.insert(normal_addr);
  }
}

ICE::Endpoint*
RtpsUdpDataLink::get_ice_endpoint() const {
  return this->impl().get_ice_endpoint();
}

} // namespace DCPS
} // namespace OpenDDS

OPENDDS_END_VERSIONED_NAMESPACE_DECL
