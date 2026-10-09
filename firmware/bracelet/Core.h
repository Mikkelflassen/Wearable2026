// Core.h - the bracelet's "brain": talking queue, new-topic queue, break vote
// and agree/disagree/comment feedback.
//
// No Arduino code in here on purpose, so the exact same logic can be compiled
// on a laptop and tested (see tests/). bracelet.ino feeds it button/snap input
// and radio packets, and plays the vibration patterns it asks for.
//
// How the bracelets agree without a "master":
//  * Every bracelet broadcasts its own state about once a second (and at once
//    when it changes):   W26 S <id> <talkTicket> <topicTicket> <pause> <epoch>
//  * Joining a queue = taking a ticket one higher than any ticket seen so far.
//    Everyone sorts by ticket, so everyone sees the same queue order.
//  * One-off signals are events:   W26 E <id> <eventId> <A|D|C>
//  * A bracelet that has been silent for 5 s is dropped from the group.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace w26 {

enum Snap : uint8_t { SNAP_NONE = 0, SNAP_TALK, SNAP_TOPIC, SNAP_PAUSE };

enum Pattern : uint8_t {
  P_NONE = 0,
  P_AGREE,       // someone agrees with you (you are the speaker)
  P_DISAGREE,    // someone disagrees with you
  P_COMMENT,     // someone has a comment on the current topic
  P_YOUR_TURN,   // you are now first in the talking queue
  P_TOPIC_TURN,  // talking queue is empty: introduce your new topic
  P_BREAK,       // more than half the group wants a break
  P_ACK          // tiny tick: your own input was registered
};

inline const char* patternName(Pattern p) {
  switch (p) {
    case P_AGREE: return "AGREE";
    case P_DISAGREE: return "DISAGREE";
    case P_COMMENT: return "COMMENT";
    case P_YOUR_TURN: return "YOUR_TURN";
    case P_TOPIC_TURN: return "TOPIC_TURN";
    case P_BREAK: return "BREAK";
    case P_ACK: return "ACK";
    default: return "NONE";
  }
}

// ---- timing you may want to tune -------------------------------------------
static const uint32_t HEARTBEAT_MS = 1000;     // how often state is re-broadcast
static const uint32_t RESEND_MS = 120;         // a change is sent twice, this far apart
static const uint32_t PEER_TIMEOUT_MS = 5000;  // silent this long = left the group
static const uint32_t TOPIC_SETTLE_MS = 1500;  // talk queue must stay empty this long
static const uint32_t TOPIC_GRACE_MS = 8000;   // time to move the snap topic -> talk
static const uint32_t EVENT_REPEAT_MS = 80;    // events are sent 3 times (no ACKs)
static const int EVENT_REPEATS = 3;
static const uint32_t FEEDBACK_MERGE_MS = 3000; // same signal again within this = still one buzz
static const int MAX_PEERS = 8;
static const int W26_LINE = 64;

class Core {
 public:
  void begin(uint16_t id, uint32_t now, uint16_t eventSeed = 0) {
    *this = Core();
    id_ = id;
    nextEid_ = eventSeed;
    sendState(now);
  }

  uint16_t id() const { return id_; }

  // ---- inputs from the wearer ---------------------------------------------
  // Where the loose snap is attached right now.
  void setSnap(Snap s, uint32_t now) {
    if (s == snap_) return;
    Snap old = snap_;
    snap_ = s;
    if (old == SNAP_TALK) talk_ = 0;
    if (old == SNAP_TOPIC) {
      if (topicNotified_ && s == SNAP_NONE) {
        // It was our turn to introduce a topic and the snap is on its way to
        // "talk". Keep our place until it arrives so the next person in the
        // topic queue is not buzzed in the gap.
        inTransit_ = true;
        transitUntil_ = now + TOPIC_GRACE_MS;
      } else {
        topic_ = 0;
        topicNotified_ = false;
      }
    }
    if (s != SNAP_NONE && inTransit_) {
      inTransit_ = false;
      topic_ = 0;
      topicNotified_ = false;
    }
    if (s == SNAP_TALK) talk_ = nextTicket();
    if (s == SNAP_TOPIC) topic_ = nextTicket();
    if (s == SNAP_PAUSE) pause_ = true;  // sticky: stays set when the snap moves on
    changed(now);
  }

  // 'A' agree, 'D' disagree, 'C' comment on the current topic (double press).
  void press(char type, uint32_t now) {
    if (type != 'A' && type != 'D' && type != 'C') return;
    if (isSpeaker()) return;  // the speaker does not signal to themselves
    for (int i = 0; i < MAX_PENDING; i++) {
      if (pending_[i].left == 0) {
        pending_[i].eid = ++nextEid_;
        pending_[i].type = type;
        pending_[i].left = EVENT_REPEATS;
        pending_[i].at = now;
        break;
      }
    }
    sendEvents(now);
  }

  // Take back your wish for a break.
  void clearPause(uint32_t now) {
    if (!pause_) return;
    pause_ = false;
    pushPattern(P_ACK);
    changed(now);
  }

  // ---- input from the radio / serial --------------------------------------
  void onLine(const char* line, uint32_t now) {
    if (strncmp(line, "W26 ", 4) != 0) return;
    if (line[4] == 'S') {
      unsigned long id, talk, topic, pause, epoch;
      if (sscanf(line + 5, "%lu %lu %lu %lu %lu", &id, &talk, &topic, &pause, &epoch) != 5) return;
      if (id == id_ || id == 0) return;
      Peer* p = peer((uint16_t)id, now);
      if (!p) return;
      p->talk = (uint32_t)talk;
      p->topic = (uint32_t)topic;
      p->pause = pause != 0;
      p->epoch = (uint32_t)epoch;
      if (p->talk > maxTicket_) maxTicket_ = p->talk;
      if (p->topic > maxTicket_) maxTicket_ = p->topic;
      if (p->epoch > epoch_) {
        // Someone else already saw the majority: the break is called.
        bool notify = synced_ || pause_;
        epoch_ = p->epoch;
        pause_ = false;
        if (notify) pushPattern(P_BREAK);
        sendState(now);
      }
      synced_ = true;
      evaluate(now);
    } else if (line[4] == 'E') {
      unsigned long id, eid;
      char type = 0;
      if (sscanf(line + 5, "%lu %lu %c", &id, &eid, &type) != 3) return;
      if (id == id_ || id == 0) return;
      Peer* p = peer((uint16_t)id, now);
      if (!p) return;
      for (int i = 0; i < p->seenN; i++)
        if (p->seen[i] == (uint16_t)eid) return;  // a repeat of one we handled
      p->seen[p->seenPos] = (uint16_t)eid;
      p->seenPos = (p->seenPos + 1) % SEEN;
      if (p->seenN < SEEN) p->seenN++;
      if (!isSpeaker()) return;  // feedback is only felt by the speaker
      // The speaker only needs to know that SOMEONE agrees/disagrees/has a
      // comment, not how many: the same signal from several people close
      // together is felt as one buzz.
      int k = type == 'A' ? 0 : type == 'D' ? 1 : type == 'C' ? 2 : -1;
      if (k < 0) return;
      if (fedBefore_[k] && now - fedAt_[k] < FEEDBACK_MERGE_MS) return;
      fedBefore_[k] = true;
      fedAt_[k] = now;
      pushPattern(k == 0 ? P_AGREE : k == 1 ? P_DISAGREE : P_COMMENT);
    }
  }

  // Call often (every loop).
  void tick(uint32_t now) {
    if (inTransit_ && (int32_t)(now - transitUntil_) >= 0) {
      inTransit_ = false;
      topic_ = 0;
      topicNotified_ = false;
      changed(now);
    }
    sendEvents(now);
    if (resendPending_ && (int32_t)(now - resendAt_) >= 0) sendState(now);
    if (now - lastStateAt_ >= HEARTBEAT_MS) sendState(now);
    evaluate(now);
  }

  // ---- outputs -------------------------------------------------------------
  // Packets to broadcast. Returns false when there is nothing left.
  bool popLine(char* out, size_t n) {
    if (outCount_ == 0) return false;
    strncpy(out, out_[outHead_], n);
    out[n - 1] = 0;
    outHead_ = (outHead_ + 1) % OUTQ;
    outCount_--;
    return true;
  }

  // Vibration patterns to play. Returns P_NONE when there is nothing left.
  Pattern popPattern() {
    if (patCount_ == 0) return P_NONE;
    Pattern p = pat_[patHead_];
    patHead_ = (patHead_ + 1) % PATQ;
    patCount_--;
    return p;
  }

  // ---- for status displays -------------------------------------------------
  bool isSpeaker() const { return talk_ > 0 && talkHead() == id_; }
  bool pauseWanted() const { return pause_; }
  Snap snap() const { return snap_; }
  int groupSize() const {
    int n = 1;
    for (int i = 0; i < MAX_PEERS; i++) if (peers_[i].used) n++;
    return n;
  }
  // 0 = not in the talking queue, 1 = speaker, 2 = next, ...
  int talkPosition() const { return position(talk_, false); }
  int topicPosition() const { return position(topic_, true); }
  int pauseVotes() const {
    int c = pause_ ? 1 : 0;
    for (int i = 0; i < MAX_PEERS; i++)
      if (peers_[i].used && peers_[i].pause && peers_[i].epoch == epoch_) c++;
    return c;
  }
  void status(char* out, size_t n) const {
    static const char* names[] = {"none", "talk", "topic", "pause"};
    snprintf(out, n, "id=%u snap=%s talkPos=%d topicPos=%d break=%d/%d%s", (unsigned)id_,
             names[snap_], talkPosition(), topicPosition(), pauseVotes(), groupSize(),
             pause_ ? " (I want a break)" : "");
  }

 private:
  static const int OUTQ = 8, PATQ = 8, SEEN = 4, MAX_PENDING = 4;

  struct Peer {
    bool used = false;
    uint16_t id = 0;
    uint32_t talk = 0, topic = 0, epoch = 0, lastSeen = 0;
    bool pause = false;
    uint16_t seen[SEEN] = {0, 0, 0, 0};
    uint8_t seenN = 0, seenPos = 0;
  };
  struct Pending {
    uint16_t eid = 0;
    char type = 0;
    uint8_t left = 0;
    uint32_t at = 0;
  };

  Peer* peer(uint16_t id, uint32_t now) {
    Peer* free_ = nullptr;
    for (int i = 0; i < MAX_PEERS; i++) {
      if (peers_[i].used && peers_[i].id == id) {
        peers_[i].lastSeen = now;
        return &peers_[i];
      }
      if (!peers_[i].used && !free_) free_ = &peers_[i];
    }
    if (free_) {
      *free_ = Peer();
      free_->used = true;
      free_->id = id;
      free_->lastSeen = now;
    }
    return free_;
  }

  uint32_t nextTicket() { return ++maxTicket_; }

  // Lower ticket first; the id breaks a tie when two people snap at once.
  static bool before(uint32_t ta, uint16_t ia, uint32_t tb, uint16_t ib) {
    return ta < tb || (ta == tb && ia < ib);
  }

  uint16_t head(bool topicQueue) const {
    uint32_t bestT = topicQueue ? topic_ : talk_;
    uint16_t bestId = bestT ? id_ : 0;
    for (int i = 0; i < MAX_PEERS; i++) {
      if (!peers_[i].used) continue;
      uint32_t t = topicQueue ? peers_[i].topic : peers_[i].talk;
      if (t == 0) continue;
      if (bestId == 0 || before(t, peers_[i].id, bestT, bestId)) {
        bestT = t;
        bestId = peers_[i].id;
      }
    }
    return bestId;
  }
  uint16_t talkHead() const { return head(false); }

  int position(uint32_t mine, bool topicQueue) const {
    if (mine == 0) return 0;
    int pos = 1;
    for (int i = 0; i < MAX_PEERS; i++) {
      if (!peers_[i].used) continue;
      uint32_t t = topicQueue ? peers_[i].topic : peers_[i].talk;
      if (t && before(t, peers_[i].id, mine, id_)) pos++;
    }
    return pos;
  }

  void changed(uint32_t now) {
    sendState(now);
    resendPending_ = true;
    resendAt_ = now + RESEND_MS;
    evaluate(now);
  }

  void evaluate(uint32_t now) {
    for (int i = 0; i < MAX_PEERS; i++)
      if (peers_[i].used && now - peers_[i].lastSeen > PEER_TIMEOUT_MS) peers_[i].used = false;

    // --- talking queue: buzz when a waiting person reaches the front
    bool speaker = isSpeaker();
    if (speaker && wasWaiting_) pushPattern(P_YOUR_TURN);
    wasWaiting_ = talk_ > 0 && !speaker;

    // --- new-topic queue: only when nobody is left in the talking queue
    if (talkHead() != 0) {
      emptyKnown_ = false;
      // Someone started talking again before we moved: wait for the next gap.
      if (topicNotified_ && snap_ == SNAP_TOPIC) topicNotified_ = false;
    } else {
      if (!emptyKnown_) {
        emptyKnown_ = true;
        emptySince_ = now;
      }
      if (snap_ == SNAP_TOPIC && topic_ > 0 && !topicNotified_ && head(true) == id_ &&
          now - emptySince_ >= TOPIC_SETTLE_MS) {
        topicNotified_ = true;
        pushPattern(P_TOPIC_TURN);
      }
    }

    // --- break vote: more than half of the bracelets that are present
    int n = groupSize();
    if (n >= 2 && pauseVotes() * 2 > n) {
      epoch_++;
      pause_ = false;
      synced_ = true;
      pushPattern(P_BREAK);
      sendState(now);
      resendPending_ = true;
      resendAt_ = now + RESEND_MS;
    }
  }

  void sendState(uint32_t now) {
    char buf[W26_LINE];
    snprintf(buf, sizeof(buf), "W26 S %u %lu %lu %d %lu", (unsigned)id_, (unsigned long)talk_,
             (unsigned long)topic_, pause_ ? 1 : 0, (unsigned long)epoch_);
    pushLine(buf);
    lastStateAt_ = now;
    resendPending_ = false;
  }

  void sendEvents(uint32_t now) {
    for (int i = 0; i < MAX_PENDING; i++) {
      Pending& e = pending_[i];
      if (e.left == 0 || (int32_t)(now - e.at) < 0) continue;
      char buf[W26_LINE];
      snprintf(buf, sizeof(buf), "W26 E %u %u %c", (unsigned)id_, (unsigned)e.eid, e.type);
      pushLine(buf);
      e.left--;
      e.at = now + EVENT_REPEAT_MS;
    }
  }

  void pushLine(const char* s) {
    if (outCount_ == OUTQ) {  // full: drop the oldest
      outHead_ = (outHead_ + 1) % OUTQ;
      outCount_--;
    }
    int tail = (outHead_ + outCount_) % OUTQ;
    strncpy(out_[tail], s, W26_LINE);
    out_[tail][W26_LINE - 1] = 0;
    outCount_++;
  }

  void pushPattern(Pattern p) {
    if (patCount_ == PATQ) return;
    pat_[(patHead_ + patCount_) % PATQ] = p;
    patCount_++;
  }

  uint16_t id_ = 0;
  Snap snap_ = SNAP_NONE;
  uint32_t talk_ = 0, topic_ = 0, epoch_ = 0, maxTicket_ = 0;
  bool pause_ = false, synced_ = false;
  bool wasWaiting_ = false;
  bool topicNotified_ = false, inTransit_ = false;
  uint32_t transitUntil_ = 0;
  bool emptyKnown_ = false;
  uint32_t emptySince_ = 0;
  uint32_t lastStateAt_ = 0, resendAt_ = 0;
  bool resendPending_ = false;
  uint16_t nextEid_ = 0;
  bool fedBefore_[3] = {false, false, false};
  uint32_t fedAt_[3] = {0, 0, 0};
  Peer peers_[MAX_PEERS];
  Pending pending_[MAX_PENDING];
  char out_[OUTQ][W26_LINE];
  int outHead_ = 0, outCount_ = 0;
  Pattern pat_[PATQ];
  int patHead_ = 0, patCount_ = 0;
};

// Text commands, so a bracelet can be driven without its buttons and snaps
// (from the serial monitor, the simulator, or the tests):
//   !talk !topic !pause !off   move the snap      !a !d !c   agree/disagree/comment
//   !unpause                   withdraw break wish !status    print state
// Returns true if the line was a command.
inline bool handleCommand(Core& core, const char* line, uint32_t now, char* reply, size_t n) {
  if (line[0] != '!') return false;
  const char* c = line + 1;
  reply[0] = 0;
  if (!strcmp(c, "talk")) core.setSnap(SNAP_TALK, now);
  else if (!strcmp(c, "topic")) core.setSnap(SNAP_TOPIC, now);
  else if (!strcmp(c, "pause")) core.setSnap(SNAP_PAUSE, now);
  else if (!strcmp(c, "off")) core.setSnap(SNAP_NONE, now);
  else if (!strcmp(c, "a")) core.press('A', now);
  else if (!strcmp(c, "d")) core.press('D', now);
  else if (!strcmp(c, "c")) core.press('C', now);
  else if (!strcmp(c, "unpause")) core.clearPause(now);
  else if (!strcmp(c, "status")) core.status(reply, n);
  else snprintf(reply, n, "unknown command: %s", line);
  return true;
}

}  // namespace w26
