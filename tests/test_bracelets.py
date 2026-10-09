#!/usr/bin/env python3
"""Scenario tests for the bracelet rules.

Runs every scenario with three bracelets in four mixes: all Python (the
simulator's logic), all C++ (the firmware's logic, compiled for the laptop) and
two mixed groups, so the simulator and the firmware are proven to behave the
same and to understand each other.

  python tests/test_bracelets.py          (needs g++ for the C++ mixes)
"""
import os
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "sim"))
from core import COMMANDS, Core  # noqa: E402

STEP = 20
NATIVE = None


class PyBracelet:
    def __init__(self, ident):
        self.core = Core(ident, 0, ident * 100)

    def step(self, now, inputs):
        for line in inputs:
            if line.startswith("!"):
                COMMANDS[line[1:]](self.core, now)
            else:
                self.core.on_line(line, now)
        self.core.tick(now)
        return self.core.pop_lines(), self.core.pop_patterns()

    def close(self):
        pass


class CppBracelet:
    def __init__(self, ident):
        self.proc = subprocess.Popen([NATIVE, str(ident)], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, text=True, bufsize=1)

    def step(self, now, inputs):
        self.proc.stdin.write("@%d\n%s?\n" % (now, "".join(l + "\n" for l in inputs)))
        self.proc.stdin.flush()
        lines, patterns = [], []
        while True:
            out = self.proc.stdout.readline().strip()
            if out == "# sync":
                return lines, patterns
            if out.startswith("W26 "):
                lines.append(out)
            elif out.startswith("# vibe "):
                patterns.append(out[7:])

    def close(self):
        self.proc.stdin.close()
        self.proc.wait()


class World:
    def __init__(self, kinds, loss=0.0):
        self.b = [(PyBracelet if k == "p" else CppBracelet)(i + 1) for i, k in enumerate(kinds)]
        self.now = 0
        self.inbox = [[] for _ in self.b]
        self.felt = [[] for _ in self.b]
        self.off_air = set()
        self.loss = loss
        self.rng = random.Random(1)

    def do(self, who, action):
        self.inbox[who].append("!" + action)

    def run(self, ms):
        for _ in range(ms // STEP):
            self.now += STEP
            sent = []
            for i, b in enumerate(self.b):
                if i in self.off_air:
                    continue
                lines, patterns = b.step(self.now, self.inbox[i])
                self.inbox[i] = []
                self.felt[i] += patterns
                sent.append((i, lines))
            for i, lines in sent:
                for j in range(len(self.b)):
                    if j != i:
                        self.inbox[j] += [l for l in lines if self.rng.random() >= self.loss]

    def expect(self, *felt):
        got = [list(f) for f in self.felt]
        self.felt = [[] for _ in self.b]
        assert got == [list(f) for f in felt], "at %d ms expected %s but felt %s" % (self.now, list(felt), got)

    def close(self):
        for b in self.b:
            b.close()


def talk_queue(w):
    w.do(0, "talk"); w.run(300)
    w.do(1, "talk"); w.run(300)
    w.do(2, "talk"); w.run(500)
    w.expect([], [], [])
    w.do(2, "a"); w.run(500); w.expect(["AGREE"], [], [])
    w.do(1, "d"); w.run(500); w.expect(["DISAGREE"], [], [])
    w.do(1, "c"); w.run(500); w.expect(["COMMENT"], [], [])
    w.do(0, "a"); w.run(500); w.expect([], [], [])  # the speaker cannot signal themselves
    w.do(0, "off"); w.run(500); w.expect([], ["YOUR_TURN"], [])
    w.do(0, "d"); w.run(500); w.expect([], ["DISAGREE"], [])  # feedback follows the speaker
    w.do(1, "off"); w.run(500); w.expect([], [], ["YOUR_TURN"])
    w.do(2, "off"); w.run(3000); w.expect([], [], [])


def many_agree_one_buzz(w):
    w.do(0, "talk"); w.run(500)
    w.do(1, "a"); w.do(2, "a"); w.run(1000); w.expect(["AGREE"], [], [])  # both at once
    w.do(1, "a"); w.run(1000); w.expect([], [], [])  # and again shortly after
    w.do(2, "d"); w.run(1000); w.expect(["DISAGREE"], [], [])  # a different signal still gets through
    w.run(3000)
    w.do(1, "a"); w.run(1000); w.expect(["AGREE"], [], [])  # later agreement buzzes again


def new_topic(w):
    w.do(0, "talk"); w.run(300)
    w.do(1, "topic"); w.run(300)
    w.do(2, "topic"); w.run(3000)
    w.expect([], [], [])  # someone is still talking
    w.do(0, "off"); w.run(1000); w.expect([], [], [])  # short settle time first
    w.run(1000); w.expect([], ["TOPIC_TURN"], [])
    w.do(1, "off"); w.run(2000); w.expect([], [], [])  # snap on its way to "talk"
    w.do(1, "talk"); w.run(3000); w.expect([], [], [])  # topic started, no buzz for 2 yet
    w.do(1, "off"); w.run(2000); w.expect([], [], ["TOPIC_TURN"])


def topic_withdrawn(w):
    w.do(0, "talk"); w.run(300)
    w.do(1, "topic"); w.run(500)
    w.do(1, "off"); w.run(500)  # changed their mind before the queue emptied
    w.do(0, "off"); w.run(4000)
    w.expect([], [], [])


def topic_never_started(w):
    w.do(1, "topic"); w.do(2, "topic"); w.run(2000)
    w.expect([], ["TOPIC_TURN"], [])
    w.do(1, "off"); w.run(7000); w.expect([], [], [])  # 1 is given time to move the snap
    w.run(4000); w.expect([], [], ["TOPIC_TURN"])  # ...but not forever


def talk_interrupts_topic(w):
    w.do(1, "topic"); w.run(2000); w.expect([], ["TOPIC_TURN"], [])
    w.do(0, "talk"); w.run(1000)  # someone jumps in on the old topic first
    w.do(0, "off"); w.run(2000); w.expect([], ["TOPIC_TURN"], [])  # buzzed again when free


def break_vote(w):
    w.run(300)
    w.do(0, "pause"); w.run(500)
    w.do(0, "talk"); w.run(2000)  # wish is remembered after moving on
    w.expect([], [], [])
    w.do(1, "pause"); w.run(1000)
    w.expect(["BREAK"], ["BREAK"], ["BREAK"])
    w.run(4000); w.expect([], [], [])  # exactly once
    w.do(2, "pause"); w.run(2000); w.expect([], [], [])  # votes start from zero again
    w.do(0, "pause"); w.run(1000)
    w.expect(["BREAK"], ["BREAK"], ["BREAK"])


def break_withdrawn(w):
    w.run(300)
    w.do(0, "pause"); w.run(500)
    w.do(0, "unpause"); w.run(500); w.expect(["ACK"], [], [])
    w.do(1, "pause"); w.run(3000); w.expect([], [], [])


def speaker_battery_dies(w):
    w.do(0, "talk"); w.run(300)
    w.do(1, "talk"); w.run(500)
    w.off_air.add(0)
    w.run(4000); w.expect([], [], [])
    w.run(2000); w.expect([], ["YOUR_TURN"], [])


def same_moment(w):
    w.do(0, "talk"); w.run(500)
    w.do(1, "talk"); w.do(2, "talk"); w.run(500)  # both snap in the same instant
    w.do(0, "off"); w.run(500); w.expect([], ["YOUR_TURN"], [])
    w.do(1, "off"); w.run(500); w.expect([], [], ["YOUR_TURN"])


def lossy_radio(w):
    w.loss = 0.3  # 30 % of all packets vanish
    w.do(0, "talk"); w.run(1500)
    w.do(1, "talk"); w.run(1500)
    w.do(2, "talk"); w.run(3000)
    w.do(0, "off"); w.run(3000); w.expect([], ["YOUR_TURN"], [])
    w.do(1, "off"); w.run(3000); w.expect([], [], ["YOUR_TURN"])
    w.do(0, "pause"); w.do(2, "pause"); w.run(4000)
    w.expect(["BREAK"], ["BREAK"], ["BREAK"])


SCENARIOS = [talk_queue, many_agree_one_buzz, new_topic, topic_withdrawn, topic_never_started, talk_interrupts_topic,
             break_vote, break_withdrawn, speaker_battery_dies, same_moment, lossy_radio]


def main():
    global NATIVE
    mixes = ["ppp"]
    if shutil.which("g++"):
        NATIVE = os.path.join(tempfile.mkdtemp(), "native_bracelet")
        subprocess.check_call(["g++", "-std=c++17", "-Wall", "-o", NATIVE,
                               os.path.join(ROOT, "tests", "native_main.cpp")])
        mixes += ["ccc", "cpp", "pcc"]
    else:
        print("g++ not found: only testing the Python simulator logic")
    failed = 0
    for mix in mixes:
        for scenario in SCENARIOS:
            w = World(mix)
            try:
                scenario(w)
                print("ok    %s  %s" % (mix, scenario.__name__))
            except AssertionError as exc:
                failed += 1
                print("FAIL  %s  %s: %s" % (mix, scenario.__name__, exc))
            finally:
                w.close()
    print("\n%d failed" % failed if failed else "\nall passed (c = firmware C++ logic, p = simulator Python logic)")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
