# SPDX-License-Identifier: Apache-2.0
#
# Read the vendor's own board module, without decompiling it.
#
# The switch carries Arista's board description as compiled Python --
# DosBoard/SantaRosaPca and friends -- and it is the most direct statement
# there is of what is on this board and in what order it is brought up. There
# is no decompiler on the bench, and there does not need to be: the runtime is
# on the switch, so every function's co_names and co_consts can simply be read.
# That is enough to see which components a routine touches, at what addresses,
# and in what order it looks them up.
#
# HOW TO READ THE OUTPUT. co_names is the order in which a function looks names
# up. It tracks call order closely and is not a transcript -- treat a sequence
# from it as strong evidence of shape rather than as a decompiled listing. The
# constants beside it are firmer: an address printed next to a component's name
# is that component's address.
#
# THIS IS FOR UNDERSTANDING, AND NOTHING IT PRINTS IS COPIED INTO NOSaic. What
# lands in the tree is what was learned -- the parts, the addresses, the order
# -- written in our own words in the board's hardware.md, the same rule the
# rest of this port follows.
#
#   copy http://<host>/introspect.py flash:introspect.py
#   bash python /mnt/flash/introspect.py DosBoard.SantaRosaPca [filter ...]
#
# Runs on the switch under EOS's Python 2.7, so it is Python 2.
import sys, types

def consts(code, depth=0):
    out = []
    for c in code.co_consts:
        if isinstance(c, (int, long)) and not isinstance(c, bool):
            out.append(hex(c) if c > 9 else str(c))
        elif isinstance(c, str) and c and len(c) < 60:
            out.append(repr(c))
    return out

def walk(obj, name, seen, want):
    code = None
    if isinstance(obj, types.FunctionType):
        code = obj.func_code
    elif isinstance(obj, types.MethodType):
        code = obj.im_func.func_code
    if code is None:
        return
    key = (name, code.co_firstlineno)
    if key in seen:
        return
    seen.add(key)
    text = " ".join(code.co_names) + " " + " ".join(consts(code))
    if want and not any(w in name.lower() or w in text.lower() for w in want):
        return
    print("--- %s(%s)" % (name, ", ".join(code.co_varnames[:code.co_argcount])))
    print("    names : %s" % " ".join(code.co_names))
    print("    consts: %s" % " ".join(consts(code)))

def dump(modname, want):
    try:
        mod = __import__(modname, {}, {}, ["x"])
    except Exception as e:
        print("!! cannot import %s: %s" % (modname, e))
        return
    print("=== %s ===" % modname)
    seen = set()
    for n in dir(mod):
        o = getattr(mod, n, None)
        if isinstance(o, types.FunctionType):
            walk(o, n, seen, want)
        elif isinstance(o, (type, types.ClassType)):
            for mn in dir(o):
                mo = getattr(o, mn, None)
                if isinstance(mo, (types.FunctionType, types.MethodType)):
                    walk(mo, "%s.%s" % (n, mn), seen, want)

want = [w for w in sys.argv[2:]]
dump(sys.argv[1], want)
