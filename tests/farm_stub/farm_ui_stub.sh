#!/bin/sh
# A stand-in for `emfdtd_farm.py` for the window's tests: canned JSON, and a record of what it was asked.
# EMFDTD_FARM_STUB_LOG (optional) gets one line per call.
[ -n "$EMFDTD_FARM_STUB_LOG" ] && echo "$*" >> "$EMFDTD_FARM_STUB_LOG"
case "$1" in
status)
  echo '{"home":"/stub/farm","enabled":{"coordinator":true,"worker":false,"client":true}}' ;;
queue)
  cat <<'JSON'
{"devices":[{"worker":"win-egpu","uuid":"99cb","name":"NVIDIA GeForce RTX 4060 Ti","driver":"NVIDIA","mcells_per_s":1832.0,"state":"busy","ok":true},
            {"worker":"mac-studio","uuid":"190a","name":"Apple M1 Pro","driver":"KosmicKrisp","mcells_per_s":1200.0,"state":"idle","ok":true}],
 "jobs":[{"job":"9cf37054e5064a39b9298f6ec19fe720","owner":"client:mac-main","label":"nd_med_01-on-RTX","state":"running","attempts":1,"worker":"win-egpu","device":"99cb","progress":"progress: 41200 steps, about 37%, 312 s","percent":37,"reason":""},
         {"job":"aaaa0000bbbb1111cccc2222dddd3333","owner":"client:mac-main","label":"patch-farm-2","state":"queued","attempts":0,"worker":null,"device":null,"progress":"","percent":null,"reason":""},
         {"job":"eeee4444ffff5555aaaa6666bbbb7777","owner":"client:mac-main","label":"old failed job","state":"failed","attempts":2,"worker":null,"device":null,"progress":"","percent":null,"reason":"worker stopped answering; gave up after 2 attempts"}]}
JSON
  ;;
members)
  echo '{"members":[{"key":"worker:win-egpu","fingerprint":"b323457cf967618f0000","may":["run"],"revoked":false},{"key":"client:mac-main","fingerprint":"672f81215c9873050000","may":["cancel","results","status","submit"],"revoked":false}]}' ;;
enable|disable|cancel|revoke)
  echo "stub: $*" ;;
invite)
  echo "invitation for $4 '$2', valid 10 minutes, one use."
  echo "  code:        ABCD-EFGH-JKLM"
  echo "  fingerprint: 1111-2222-3333-4444"
  echo "  listening on 192.0.2.1:7421 until it is used"
  sleep 30 ;;
*)
  echo "stub: unknown command $*" >&2; exit 2 ;;
esac
