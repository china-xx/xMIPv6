//
// Copyright (C) 2004 Andras Varga
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//


#include <string.h>
#include "TCPMain.h"
#include "TCPConnection.h"
#include "TCPSegment_m.h"
#include "TCPCommand_m.h"
#include "IPControlInfo_m.h"
#include "TCPSendQueue.h"
#include "TCPReceiveQueue.h"
#include "TCPAlgorithm.h"

bool TCPConnection::tryFastRoute(TCPSegment *tcpseg)
{
    // fast route processing not yet implemented
    return false;
}


void TCPConnection::segmentArrivalWhileClosed(TCPSegment *tcpseg, IPAddress srcAddr, IPAddress destAddr)
{
    ev << "Seg arrived: ";
    printSegmentBrief(tcpseg);

    ev << "Segment doesn't belong to any existing connection\n";

    // RFC 793:
    //"
    // all data in the incoming segment is discarded.  An incoming
    // segment containing a RST is discarded.  An incoming segment not
    // containing a RST causes a RST to be sent in response.  The
    // acknowledgment and sequence field values are selected to make the
    // reset sequence acceptable to the TCP that sent the offending
    // segment.
    //
    // If the ACK bit is off, sequence number zero is used,
    //
    //    <SEQ=0><ACK=SEG.SEQ+SEG.LEN><CTL=RST,ACK>
    //
    // If the ACK bit is on,
    //
    //    <SEQ=SEG.ACK><CTL=RST>
    //"
    if (tcpseg->rstBit())
    {
        ev << "RST bit set: dropping segment\n";
        return;
    }

    if (!tcpseg->ackBit())
    {
        ev << "ACK bit not set: sending RST+ACK\n";
        uint32 ackNo = tcpseg->sequenceNo() + (uint32)tcpseg->payloadLength();
        sendRstAck(0,ackNo,destAddr,srcAddr,tcpseg->destPort(),tcpseg->srcPort());
    }
    else
    {
        ev << "ACK bit set: sending RST\n";
        sendRst(tcpseg->ackNo(),destAddr,srcAddr,tcpseg->destPort(),tcpseg->srcPort());
    }
}

TCPEventCode TCPConnection::process_RCV_SEGMENT(TCPSegment *tcpseg, IPAddress src, IPAddress dest)
{
    ev << "Seg arrived: ";
    printSegmentBrief(tcpseg);

    //
    // Note: this code is organized exactly as RFC 793, section "3.9 Event
    // Processing", subsection "SEGMENT ARRIVES".
    //

    if (fsm.state()==TCP_S_LISTEN)
    {
        return processSegmentInListen(tcpseg, src, dest);
    }
    if (fsm.state()==TCP_S_SYN_SENT)
    {
        return processSegmentInSynSent(tcpseg, src, dest);
    }
    else
    {
        //
        // RFC 793 steps "first check sequence number", "second check the RST bit", etc
        //
        return processSegment1stThru8th(tcpseg);
    }
}

TCPEventCode TCPConnection::processSegment1stThru8th(TCPSegment *tcpseg)
{
    //
    // RFC 793: first check sequence number
    //
    bool isSegmentAcceptable = checkSegmentSeqNum(tcpseg);
    if (!isSegmentAcceptable)
    {
        //"
        // If an incoming segment is not acceptable, an acknowledgment
        // should be sent in reply (unless the RST bit is set, if so drop
        // the segment and return):
        //
        //  <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
        //"
        if (tcpseg->rstBit())
        {
            ev << "RST with unacceptable seqNum: dropping\n";
        }
        else
        {
            ev << "Segment not acceptable, sending ACK with current receive seq\n";
            sendAck();
        }
        delete tcpseg;
        return TCP_E_IGNORE;
    }

    //
    // RFC 793: second check the RST bit,
    //
    if (tcpseg->rstBit())
    {
        // Note: no need to handle justReached_SYN_RCVD_from_LISTEN here, because
        // processSegmentInListen() already handles RST.

        switch (fsm.state())
        {
            case TCP_S_SYN_RCVD:
                //"
                // If this connection was initiated with a passive OPEN (i.e.,
                // came from the LISTEN state), then return this connection to
                // LISTEN state and return.  The user need not be informed.  If
                // this connection was initiated with an active OPEN (i.e., came
                // from SYN-SENT state) then the connection was refused, signal
                // the user "connection refused".  In either case, all segments
                // on the retransmission queue should be removed.  And in the
                // active OPEN case, enter the CLOSED state and delete the TCB,
                // and return.
                //"
                return processRstInSynReceived(tcpseg);

            case TCP_S_ESTABLISHED:
            case TCP_S_FIN_WAIT_1:
            case TCP_S_FIN_WAIT_2:
            case TCP_S_CLOSE_WAIT:
                //"
                // If the RST bit is set then, any outstanding RECEIVEs and SEND
                // should receive "reset" responses.  All segment queues should be
                // flushed.  Users should also receive an unsolicited general
                // "connection reset" signal.
                //
                // Enter the CLOSED state, delete the TCB, and return.
                //"
                ev << "RST: performing connection reset, closing connection\n";
                doConnectionReset();
                delete tcpseg;
                return TCP_E_RCV_RST;  // this will trigger state transition

            case TCP_S_CLOSING:
            case TCP_S_LAST_ACK:
            case TCP_S_TIME_WAIT:
                //"
                // enter the CLOSED state, delete the TCB, and return.
                //"
                ev << "RST: closing connection\n";
                delete tcpseg;
                return TCP_E_RCV_RST; // this will trigger state transition

            default: ASSERT(0);
        }
    }

    // RFC 793: third check security and precedence
    // This step is ignored.

    //
    // RFC 793: fourth, check the SYN bit,
    //
    if (tcpseg->synBit())
    {
        //"
        // If the SYN is in the window it is an error, send a reset, any
        // outstanding RECEIVEs and SEND should receive "reset" responses,
        // all segment queues should be flushed, the user should also
        // receive an unsolicited general "connection reset" signal, enter
        // the CLOSED state, delete the TCB, and return.
        //
        // If the SYN is not in the window this step would not be reached
        // and an ack would have been sent in the first step (sequence
        // number check).
        //"

        ev << "Unexpected SYN: performing connection reset, closing connection\n";
        // FIXME ASSERT( SYN in window );
        doConnectionReset();
        return TCP_E_RCV_UNEXP_SYN;
    }

    //
    // RFC 793: fifth check the ACK field,
    //
    if (!tcpseg->ackBit())
    {
        // if the ACK bit is off drop the segment and return
        ev << "ACK not set, dropping segment\n";
        delete tcpseg;
        return TCP_E_IGNORE;
    }

    TCPEventCode event = TCP_E_IGNORE;

    if (fsm.state()==TCP_S_SYN_RCVD)
    {
        //"
        // If SND.UNA =< SEG.ACK =< SND.NXT then enter ESTABLISHED state
        // and continue processing.
        //
        // If the segment acknowledgment is not acceptable, form a
        // reset segment,
        //
        //  <SEQ=SEG.ACK><CTL=RST>
        //
        // and send it.
        //"
        if (!seqLE(state->snd_una,tcpseg->ackNo()) || !seqLE(tcpseg->ackNo(),state->snd_nxt))
        {
            sendRst(tcpseg->ackNo());
            return TCP_E_IGNORE;
        }
        event = TCP_E_RCV_ACK; // will trigger transition to ESTABLISHED
        tcpMain->cancelEvent(connEstabTimer);
    }

    uint32 old_snd_nxt = state->snd_nxt; // later we'll need to see if snd_nxt changed
    if (fsm.state()==TCP_S_SYN_RCVD || fsm.state()==TCP_S_ESTABLISHED ||
        fsm.state()==TCP_S_FIN_WAIT_1 || fsm.state()==TCP_S_FIN_WAIT_2 ||
        fsm.state()==TCP_S_CLOSE_WAIT || fsm.state()==TCP_S_CLOSING)
    {
        //
        // ESTABLISHED processing:
        //"
        //  If SND.UNA < SEG.ACK =< SND.NXT then, set SND.UNA <- SEG.ACK.
        //  Any segments on the retransmission queue which are thereby
        //  entirely acknowledged are removed.  Users should receive
        //  positive acknowledgments for buffers which have been SENT and
        //  fully acknowledged (i.e., SEND buffer should be returned with
        //  "ok" response).  If the ACK is a duplicate
        //  (SEG.ACK < SND.UNA), it can be ignored.  If the ACK acks
        //  something not yet sent (SEG.ACK > SND.NXT) then send an ACK,
        //  drop the segment, and return.
        //
        //  If SND.UNA < SEG.ACK =< SND.NXT, the send window should be
        //  updated.  If (SND.WL1 < SEG.SEQ or (SND.WL1 = SEG.SEQ and
        //  SND.WL2 =< SEG.ACK)), set SND.WND <- SEG.WND, set
        //  SND.WL1 <- SEG.SEQ, and set SND.WL2 <- SEG.ACK.
        //
        //  Note that SND.WND is an offset from SND.UNA, that SND.WL1
        //  records the sequence number of the last segment used to update
        //  SND.WND, and that SND.WL2 records the acknowledgment number of
        //  the last segment used to update SND.WND.  The check here
        //  prevents using old segments to update the window.
        //"
        processAckInEstabEtc(tcpseg);
    }

    if ((fsm.state()==TCP_S_FIN_WAIT_1 && state->fin_ack_rcvd) || fsm.state()==TCP_S_FIN_WAIT_2)
    {
        //"
        // FIN-WAIT-1 STATE
        //   In addition to the processing for the ESTABLISHED state, if
        //   our FIN is now acknowledged then enter FIN-WAIT-2 and continue
        //   processing in that state.
        //
        // FIN-WAIT-2 STATE
        //  In addition to the processing for the ESTABLISHED state, if
        //  the retransmission queue is empty, the user's CLOSE can be
        //  acknowledged ("ok") but do not delete the TCB.
        //"

        if (fsm.state()==TCP_S_FIN_WAIT_1)
            event = TCP_E_RCV_ACK;  // will trigger transition to FIN-WAIT-2

        // if the retransmission queue is empty, the user's CLOSE can be
        // acknowledged ("ok")

        //FIXME tbd
        //if (sendQueue->bytesAvailable(...))
        //     indicate Closed to user
    }

    if (fsm.state()==TCP_S_CLOSING)
    {
        //"
        // In addition to the processing for the ESTABLISHED state, if
        // the ACK acknowledges our FIN then enter the TIME-WAIT state,
        // otherwise ignore the segment.
        //"
        if (state->fin_ack_rcvd)
        {
            ev << "Our FIN acked -- can go to TIME_WAIT now\n";
            event = TCP_E_RCV_ACK;  // will trigger transition to TIME-WAIT
        }
    }

    if (fsm.state()==TCP_S_LAST_ACK)
    {
        //"
        // The only thing that can arrive in this state is an
        // acknowledgment of our FIN.  If our FIN is now acknowledged,
        // delete the TCB, enter the CLOSED state, and return.
        //"
        if (state->send_fin && tcpseg->ackNo()==state->snd_fin_seq+1)
        {
            ev << "Last ACK arrived\n";
            return TCP_E_RCV_ACK;
        }
    }

    if (fsm.state()==TCP_S_TIME_WAIT)
    {
        //"
        // The only thing that can arrive in this state is a
        // retransmission of the remote FIN.  Acknowledge it, and restart
        // the 2 MSL timeout.
        //"
        //FIXME tbd
    }

    //
    // RFC 793: sixth, check the URG bit,
    //
    if (tcpseg->urgBit() && (fsm.state()==TCP_S_ESTABLISHED || fsm.state()==TCP_S_FIN_WAIT_1 ||
        fsm.state()==TCP_S_FIN_WAIT_2))
    {
        //"
        // If the URG bit is set, RCV.UP <- max(RCV.UP,SEG.UP), and signal
        // the user that the remote side has urgent data if the urgent
        // pointer (RCV.UP) is in advance of the data consumed.  If the
        // user has already been signaled (or is still in the "urgent
        // mode") for this continuous sequence of urgent data, do not
        // signal the user again.
        //"
        processUrgInEstabEtc(tcpseg);
    }

    //
    // RFC 793: seventh, process the segment text,
    //
    uint32 old_rcv_nxt = state->rcv_nxt; // if rcv_nxt changes, we need to send/schedule an ACK
    if (fsm.state()==TCP_S_ESTABLISHED || fsm.state()==TCP_S_FIN_WAIT_1 || fsm.state()==TCP_S_FIN_WAIT_2)
    {
        //"
        // Once in the ESTABLISHED state, it is possible to deliver segment
        // text to user RECEIVE buffers.  Text from segments can be moved
        // into buffers until either the buffer is full or the segment is
        // empty.  If the segment empties and carries an PUSH flag, then
        // the user is informed, when the buffer is returned, that a PUSH
        // has been received.
        //
        // When the TCP takes responsibility for delivering the data to the
        // user it must also acknowledge the receipt of the data.
        //
        // Once the TCP takes responsibility for the data it advances
        // RCV.NXT over the data accepted, and adjusts RCV.WND as
        // apporopriate to the current buffer availability.  The total of
        // RCV.NXT and RCV.WND should not be reduced.
        //
        // Please note the window management suggestions in section 3.7.
        //
        // Send an acknowledgment of the form:
        //
        //   <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
        //
        // This acknowledgment should be piggybacked on a segment being
        // transmitted if possible without incurring undue delay.
        //"
        if (tcpseg->payloadLength()>0)
        {
            ev << "Processing segment text in a data transfer state\n";

            // insert into receive buffers. If this segment is contiguous with previously
            // received ones, rcv_nxt can be increased; otherwise it stays the same but
            // the data are stored nevertheless (to avoid "Failure to retain above-sequence
            // data" problem, RFC 2525 section 2.5).
            state->rcv_nxt = receiveQueue->insertBytesFromSegment(tcpseg);

            // forward data to app
            cMessage *msg;
            while ((msg=receiveQueue->extractBytesUpTo(state->rcv_nxt))!=NULL)
            {
                sendToApp(msg);
            }

            // if this segment "filled the gap" until the previously arrived segment
            // that carried a FIN (i.e.rcv_nxt==rcv_fin_seq), we have to advance
            // rcv_nxt over the FIN.
            if (state->fin_rcvd && state->rcv_nxt==state->rcv_fin_seq)
            {
                ev << "All segments arrived up the FIN segment, advancing rcv_nxt over the FIN\n";
                state->rcv_nxt = state->rcv_fin_seq+1;
            }
        }
    }

    //
    // RFC 793: eighth, check the FIN bit,
    //
    if (tcpseg->finBit())
    {
        //"
        // If the FIN bit is set, signal the user "connection closing" and
        // return any pending RECEIVEs with same message, advance RCV.NXT
        // over the FIN, and send an acknowledgment for the FIN.  Note that
        // FIN implies PUSH for any segment text not yet delivered to the
        // user.
        //"

        // Note: seems like RFC 793 is not entirely correct here: if the
        // segment is "above sequence" (ie. RCV.NXT < SEG.SEQ), we cannot
        // advance RCV.NXT over the FIN. Instead we remember this sequence
        // number and do it later.
        uint32 fin_seq = (uint32)tcpseg->sequenceNo() + (uint32)tcpseg->payloadLength();
        if (state->rcv_nxt==fin_seq)
        {
            // advance rcv_nxt over FIN now
            ev << "FIN arrived, advancing rcv_nxt over the FIN\n";
            state->rcv_nxt++;
        }
        else
        {
            // we'll have to do it later (when an arriving segment "fills the gap")
            ev << "FIN segment above sequence, storing sequence number of FIN\n";
            state->fin_rcvd = true;
            state->rcv_fin_seq = fin_seq;
        }

        // FIXME do PUSH stuff

        // state transitions will be done in the state machine, here we just set
        // the proper event code (TCP_E_RCV_FIN or TCP_E_RCV_FIN_ACK)
        event = TCP_E_RCV_FIN;
        switch (fsm.state())
        {
            case TCP_S_FIN_WAIT_1:
                if (state->fin_ack_rcvd)
                {
                    event = TCP_E_RCV_FIN_ACK;
                    // start the time-wait timer, turn off the other timers;
                    scheduleTimeout(the2MSLTimer, TCP_TIMEOUT_2MSL);
                    tcpMain->cancelEvent(finWait2Timer);
                }
                break;
            case TCP_S_FIN_WAIT_2:
                // Start the time-wait timer, turn off the other timers.
                scheduleTimeout(the2MSLTimer, TCP_TIMEOUT_2MSL);
                tcpMain->cancelEvent(finWait2Timer);
                break;
            case TCP_S_TIME_WAIT:
                // Restart the 2 MSL time-wait timeout.
                tcpMain->cancelEvent(the2MSLTimer);
                scheduleTimeout(the2MSLTimer, TCP_TIMEOUT_2MSL);
                break;
        }
    }

    if (old_rcv_nxt!=state->rcv_nxt)
    {
        // if rcv_nxt changed, either because we received segment text or we
        // received a FIN that needs to be acked (or both), we need to send or
        // schedule an ACK.

        // tcpAlgorithm decides when and how to do ACKs
        tcpAlgorithm->receiveSeqChanged();
    }

    if (fsm.state()==TCP_S_CLOSE_WAIT && state->send_fin &&
        state->snd_nxt==state->snd_fin_seq+1 && old_snd_nxt!=state->snd_nxt)
    {
        // if we're in CLOSE_WAIT and we just got to sent our long-pending FIN,
        // we simulate a CLOSE command now (we had to defer it at that time because
        // we still had data in the send queue.) This CLOSE will take us into the
        // LAST_ACK state.
        ev << "Now we can do the CLOSE which was deferred a while ago\n";
        event = TCP_E_CLOSE;
    }

    delete tcpseg;
    return event;
}

//----

TCPEventCode TCPConnection::processSegmentInListen(TCPSegment *tcpseg, IPAddress srcAddr, IPAddress destAddr)
{
    ev << "Processing segment in LISTEN\n";

    //"
    // first check for an RST
    //   An incoming RST should be ignored.  Return.
    //"
    if (tcpseg->rstBit())
    {
        ev << "RST bit set: dropping segment\n";
        return TCP_E_IGNORE;
    }

    //"
    // second check for an ACK
    //    Any acknowledgment is bad if it arrives on a connection still in
    //    the LISTEN state.  An acceptable reset segment should be formed
    //    for any arriving ACK-bearing segment.  The RST should be
    //    formatted as follows:
    //
    //      <SEQ=SEG.ACK><CTL=RST>
    //
    //    Return.
    //"
    if (tcpseg->ackBit())
    {
        ev << "ACK bit set: dropping segment and sending RST\n";
        sendRst(tcpseg->ackNo(),destAddr,srcAddr,tcpseg->destPort(),tcpseg->srcPort());
        return TCP_E_IGNORE;
    }

    //"
    // third check for a SYN
    //"
    if (tcpseg->synBit())
    {
        if (tcpseg->finBit())
        {
            // Looks like implementations vary on how to react to SYN+FIN.
            // Some treat it as plain SYN (and replay with SYN+ACK), some send RST+ACK.
            // Let's just do the former here.
            ev << "SYN+FIN received: ignoring FIN\n";
        }

        ev << "SYN bit set: filling in foreign socket and sending SYN+ACK\n";

        //"
        // If the listen was not fully specified (i.e., the foreign socket was not
        // fully specified), then the unspecified fields should be filled in now.
        //"
        tcpMain->updateSockPair(this, destAddr, srcAddr, tcpseg->destPort(), tcpseg->srcPort());

        //"
        //  Set RCV.NXT to SEG.SEQ+1, IRS is set to SEG.SEQ and any other
        //  control or text should be queued for processing later.  ISS
        //  should be selected and a SYN segment sent of the form:
        //
        //    <SEQ=ISS><ACK=RCV.NXT><CTL=SYN,ACK>
        //
        //  SND.NXT is set to ISS+1 and SND.UNA to ISS.  The connection
        //  state should be changed to SYN-RECEIVED.
        //"
        state->rcv_nxt = tcpseg->sequenceNo()+1;
        state->irs = tcpseg->sequenceNo();
        selectInitialSeqNum();
        sendSynAck();

        //"
        // Note that any other incoming control or data (combined with SYN)
        // will be processed in the SYN-RECEIVED state, but processing of SYN
        // and ACK should not be repeated.
        //"

        // FIXME it is not clear what exactly is there left to do: RST, SYN, ACK
        // got processed already; About FIN processing see above. URG bit and
        // segment text are not processed in SYN_RCVD yet -- queue them up?

        return TCP_E_RCV_SYN;
    }

    //"
    //  fourth other text or control
    //   So you are unlikely to get here, but if you do, drop the segment, and return.
    //"
    ev << "Unexpected segment: dropping it\n";
    return TCP_E_IGNORE;
}

TCPEventCode TCPConnection::processSegmentInSynSent(TCPSegment *tcpseg, IPAddress srcAddr, IPAddress destAddr)
{
    ev << "Processing segment in SYN_SENT\n";

    //"
    // first check the ACK bit
    //
    //   If the ACK bit is set
    //
    //     If SEG.ACK =< ISS, or SEG.ACK > SND.NXT, send a reset (unless
    //     the RST bit is set, if so drop the segment and return)
    //
    //       <SEQ=SEG.ACK><CTL=RST>
    //
    //     and discard the segment.  Return.
    //
    //     If SND.UNA =< SEG.ACK =< SND.NXT then the ACK is acceptable.
    //"
    if (tcpseg->ackBit())
    {
        if (seqLE(tcpseg->ackNo(),state->iss) || seqGreater(tcpseg->ackNo(),state->snd_nxt))
        {
            ev << "ACK bit set but wrong AckNo, sending RST\n";
            sendRst(tcpseg->ackNo(),destAddr,srcAddr,tcpseg->destPort(),tcpseg->srcPort());
            return TCP_E_IGNORE;
        }
        ev << "ACK bit set, AckNo acceptable\n";
    }

    //"
    // second check the RST bit
    //
    //   If the RST bit is set
    //
    //     If the ACK was acceptable then signal the user "error:
    //     connection reset", drop the segment, enter CLOSED state,
    //     delete TCB, and return.  Otherwise (no ACK) drop the segment
    //     and return.
    //"
    if (tcpseg->rstBit())
    {
        if (tcpseg->ackBit())
        {
            ev << "RST+ACK: performing connection reset\n";
            doConnectionReset();
            return TCP_E_RCV_RST;
        }
        else
        {
            ev << "RST without ACK: dropping segment\n";
            return TCP_E_IGNORE;
        }
    }

    //"
    // third check the security and precedence -- not done
    //
    // fourth check the SYN bit
    //
    //   This step should be reached only if the ACK is ok, or there is
    //   no ACK, and it the segment did not contain a RST.
    //
    //   If the SYN bit is on and the security/compartment and precedence
    //   are acceptable then,
    //"
    if (tcpseg->synBit())
    {
        //
        //   RCV.NXT is set to SEG.SEQ+1, IRS is set to
        //   SEG.SEQ.  SND.UNA should be advanced to equal SEG.ACK (if there
        //   is an ACK), and any segments on the retransmission queue which
        //   are thereby acknowledged should be removed.
        //
        state->rcv_nxt = tcpseg->sequenceNo()+1;
        state->irs = tcpseg->sequenceNo();

        if (tcpseg->ackBit())
        {
            state->snd_una = tcpseg->ackNo();
            sendQueue->discardUpTo(state->snd_una);

            // although not mentioned in RFC 793, seems like we have to update
            // our snd_wnd from the segment here.
            ev << "Updating send window from SYN+ACK segment: wnd="<< tcpseg->window() << "\n";
            state->snd_wnd = tcpseg->window();
            state->snd_wl1 = tcpseg->sequenceNo();
            state->snd_wl2 = tcpseg->ackNo();
        }

        //"
        //   If SND.UNA > ISS (our SYN has been ACKed), change the connection
        //   state to ESTABLISHED, form an ACK segment
        //
        //     <SEQ=SND.NXT><ACK=RCV.NXT><CTL=ACK>
        //
        //   and send it.  Data or controls which were queued for
        //   transmission may be included.  If there are other controls or
        //   text in the segment then continue processing at the sixth step
        //   below where the URG bit is checked, otherwise return.
        //"
        if (seqGreater(state->snd_una, state->iss))
        {
            ev << "SYN bit set: sending ACK\n";
            sendAck();   // FIXME may include data!
            // FIXME continue processing at the sixth step below where the URG bit is checked
            tcpMain->cancelEvent(connEstabTimer);
            return TCP_E_RCV_SYN_ACK; // this will trigger transition to ESTABLISHED
        }

        //"
        //   Otherwise enter SYN-RECEIVED, form a SYN,ACK segment
        //
        //     <SEQ=ISS><ACK=RCV.NXT><CTL=SYN,ACK>
        //
        //   and send it.  If there are other controls or text in the
        //   segment, queue them for processing after the ESTABLISHED state
        //   has been reached, return.
        //"
        ev << "SYN bit set: sending SYN+ACK\n";
        state->snd_nxt = state->iss;  // FIXME is this OK?
        sendSynAck();
        // FIXME TBD: If there are other controls or text in the segment, queue them
        // for processing after the ESTABLISHED state has been reached
        return TCP_E_RCV_SYN;
    }

    //"
    // fifth, if neither of the SYN or RST bits is set then drop the
    // segment and return.
    //"
    return TCP_E_IGNORE;
}

TCPEventCode TCPConnection::processRstInSynReceived(TCPSegment *tcpseg)
{
    ev << "Processing RST in SYN_RCVD\n";

    //"
    // If this connection was initiated with a passive OPEN (i.e.,
    // came from the LISTEN state), then return this connection to
    // LISTEN state and return.  The user need not be informed.  If
    // this connection was initiated with an active OPEN (i.e., came
    // from SYN-SENT state) then the connection was refused, signal
    // the user "connection refused".  In either case, all segments
    // on the retransmission queue should be removed.  And in the
    // active OPEN case, enter the CLOSED state and delete the TCB,
    // and return.
    //"

    sendQueue->discardUpTo(sendQueue->bufferEndSeq()); // flush send queue

    if (state->active)
    {
        // FIXME TBD: signal "connection refused"
    }

    // on RCV_RST, FSM will go either to LISTEN or to CLOSED, depending on state->active
    return TCP_E_RCV_RST;
}

void TCPConnection::doConnectionReset()
{
    //"
    // Any outstanding RECEIVEs and SEND should receive "reset" responses.
    // All segment queues should be flushed.  Users should also receive an
    // unsolicited general "connection reset" signal.
    //"

    // FIXME notify user. nothing else to do.
}

void TCPConnection::processAckInEstabEtc(TCPSegment *tcpseg)
{
    ev << "Processing ACK in a data transfer state\n";

    //
    //"
    //  If SND.UNA < SEG.ACK =< SND.NXT then, set SND.UNA <- SEG.ACK.
    //  Any segments on the retransmission queue which are thereby
    //  entirely acknowledged are removed.  Users should receive
    //  positive acknowledgments for buffers which have been SENT and
    //  fully acknowledged (i.e., SEND buffer should be returned with
    //  "ok" response).  If the ACK is a duplicate
    //  (SEG.ACK < SND.UNA), it can be ignored.  If the ACK acks
    //  something not yet sent (SEG.ACK > SND.NXT) then send an ACK,
    //  drop the segment, and return.
    //
    //  If SND.UNA < SEG.ACK =< SND.NXT, the send window should be
    //  updated.  If (SND.WL1 < SEG.SEQ or (SND.WL1 = SEG.SEQ and
    //  SND.WL2 =< SEG.ACK)), set SND.WND <- SEG.WND, set
    //  SND.WL1 <- SEG.SEQ, and set SND.WL2 <- SEG.ACK.
    //
    //  Note that SND.WND is an offset from SND.UNA, that SND.WL1
    //  records the sequence number of the last segment used to update
    //  SND.WND, and that SND.WL2 records the acknowledgment number of
    //  the last segment used to update SND.WND.  The check here
    //  prevents using old segments to update the window.
    //"
    if (seqLess(state->snd_una, tcpseg->ackNo()) && seqLE(tcpseg->ackNo(), state->snd_nxt))
    {
        // ack in window.
        state->snd_una = tcpseg->ackNo();
        uint32 discardUpToSeq = state->snd_una;

        // our FIN acked?
        if (state->send_fin && tcpseg->ackNo()==state->snd_fin_seq+1)
        {
            // set flag that our FIN has been acked
            ev << "ACK acks our FIN\n";
            state->fin_ack_rcvd = true;
            discardUpToSeq--; // the FIN sequence number is not real data
        }

        sendQueue->discardUpTo(discardUpToSeq);

        if (seqLess(state->snd_wl1, tcpseg->sequenceNo()) ||
            (state->snd_wl1==tcpseg->sequenceNo() && seqLE(state->snd_wl2, tcpseg->ackNo())))
        {
            // send window should be updated
            ev << "Updating send window from segment: wnd=" << tcpseg->window() << "\n";
            state->snd_wnd = tcpseg->window();
            state->snd_wl1 = tcpseg->sequenceNo();
            state->snd_wl2 = tcpseg->ackNo();
        }

        tcpAlgorithm->receivedAck();
    }
    else if (seqGreater(tcpseg->ackNo(), state->snd_nxt))
    {
        // send an ACK, drop the segment, and return.
        ev << "ACK acks something not yet sent\n";
        tcpAlgorithm->receivedAckForDataNotYetSent(tcpseg->ackNo());
        // FIXME todo: drop segment & return
    }
}


void TCPConnection::processUrgInEstabEtc(TCPSegment *tcpseg)
{
    ev << "Processing URG in a data transfer state\n";
    ev << "FIXME not implemented yet\n";
}

void TCPConnection::processSegmentTextInEstabEtc(TCPSegment *tcpseg)
{
    ev << "Processing segment text in a data transfer state\n";
    // FIXME this function not used currently
}

//----

void TCPConnection::process_TIMEOUT_CONN_ESTAB()
{
    switch(fsm.state())
    {
        case TCP_S_SYN_RCVD:
        case TCP_S_SYN_SENT:
            // Nothing to do here. The TIMEOUT_CONN_ESTAB event will automatically
            // take the connection to CLOSED.
            break;
        default:
            // We should not receive this timeout in this state.
            opp_error("Internal error: received CONN_ESTAB timeout in state %s", stateName(fsm.state()));
    }
}

void TCPConnection::process_TIMEOUT_2MSL()
{
    //"
    // If the time-wait timeout expires on a connection delete the TCB,
    // enter the CLOSED state and return.
    //"
    switch(fsm.state())
    {
        case TCP_S_TIME_WAIT:
            // Nothing to do here. The TIMEOUT_2MSL event will automatically take
            // the connection to CLOSED.
            break;
        default:
            // We should not receive this timeout in this state.
            opp_error("Internal error: received time-wait (2MSL) timeout in state %s", stateName(fsm.state()));
    }

}

void TCPConnection::process_TIMEOUT_FIN_WAIT_2()
{
    switch(fsm.state())
    {
        case TCP_S_FIN_WAIT_2:
            // Nothing to do here. The TIMEOUT_FIN_WAIT_2 event will automatically take
            // the connection to CLOSED.
            break;
        default:
            // We should not receive this timeout in this state.
            opp_error("Internal error: received FIN_WAIT_2 timeout in state %s", stateName(fsm.state()));
    }
}

//
//FIXME TBD:
//"
// USER TIMEOUT
//
//    For any state if the user timeout expires, flush all queues, signal
//    the user "error:  connection aborted due to user timeout" in general
//    and for any outstanding calls, delete the TCB, enter the CLOSED
//    state and return.
//"
