Feature: The agent window decides everything in a headless controller, in words, through the command line

  Scenario Outline: The status is said in words
    Given an agent status <json>
    Then the panel says "<state>" and "<line>"

    Examples:
      | json                                                                                                  | state   | line                                              |
      | {"running": false}                                                                                    | Stopped | (nothing more)                                    |
      | {"running": true, "uptime_s": 45, "connected": true}                                                  | Running | Running for 45 s                                  |
      | {"running": true, "uptime_s": 7500, "connected": false}                                               | Running | Not connected to the coordinator                  |
      | {"running": true, "job": {"id": "abcdef012345", "label": "fd_dk42", "phase": "solving", "percent": 37}} | Running | Job: fd_dk42 - solving (about 37%)               |
      | {"running": true, "last_error": "coordinator refused the certificate"}                                | Running | Last problem: coordinator refused the certificate |

  Scenario: An unreadable status is reported, never a crash
    Given an agent status not json at all
    Then the panel says "Unknown" and "was not understood"

  Scenario: A percentage outside 0 to 100 is clamped
    Given an agent status {"running": true, "job": {"id": "a", "label": "x", "percent": 250}}
    Then the progress is 100

  Scenario: Stopping a worker that has a job asks first and says what happens
    Given an agent status {"running": true, "job": {"id": "abcdef012345", "label": "fd_dk42"}}
    Then stopping asks a question that mentions "cancels that job" and "fd_dk42"

  Scenario: Stopping an idle worker does not ask
    Given an agent status {"running": true}
    Then stopping asks nothing

  Scenario: Settings are checked in plain words before saving
    Given settings with no solver, no GPU and a lock command mode with no lock command
    Then the problems mention "emfdtd program", "GPU" and "lock command"

  Scenario Outline: A number setting must be above zero
    Given valid settings with <field> set to "<value>"
    Then the settings are <verdict>

    Examples:
      | field       | value | verdict |
      | max_minutes | 45    | valid   |
      | max_minutes | 0     | refused |
      | max_steps   | -3    | refused |
      | watchdog    | abc   | refused |
      | max_input_mb| 64.5  | valid   |

  Scenario: Settings go to the command line as a document on standard input
    Given valid settings
    When the settings are saved
    Then the command was "agent configure --json-in" with numbers as numbers and the GPU as a list

  Scenario: Saving settings that are not valid never reaches the command line
    Given settings with no solver, no GPU and a lock command mode with no lock command
    When the settings are saved
    Then nothing was run

  Scenario: The one-time code is never kept
    Given a controller with a fake agent
    When enrol is run with the code "ABCD-EFGH-JKLM"
    Then the code appears in no recorded command and no history entry

  Scenario: The wizard opens one step at a time
    Given a wizard on a machine with TLS 1.3
    Then only step "python" can run
    When step "python" is run
    Then step "role" can run and step "enroll" cannot

  Scenario: A machine without TLS 1.3 stops at the first step and says what to do
    Given a wizard on a machine without TLS 1.3
    When step "python" is run
    Then the step failed with "no TLS 1.3" and step "role" cannot run

  Scenario: The role step needs the confirmation
    Given a wizard with step "python" done
    When step "role" is run without confirming
    Then the step failed with "Confirm" and nothing was run
    When step "role" is run confirming
    Then the command was "enable worker --yes"

  Scenario Outline: Enrolment addresses must look like host:port
    Given a wizard with steps "python" and "role" done
    When enrol is run with invitation "<invitation>" and coordinator "<coordinator>"
    Then the enrol step is <verdict>

    Examples:
      | invitation        | coordinator       | verdict |
      | 192.168.1.20:7421 | 192.168.1.20:7420 | run     |
      | 192.168.1.20      | 192.168.1.20:7420 | refused |
      | 192.168.1.20:7421 | host              | refused |
      | box.local:99999   | box.local:7420    | refused |
      | :7421             | box.local:7420    | refused |

  Scenario: Recording devices for a coordinator-held turn needs you to hold the turn
    Given a wizard with steps "python", "role" and "enroll" done
    When devices are recorded for the coordinator-held mode without holding the turn
    Then the step failed with "Hold the GPU's turn" and nothing was run
    When devices are recorded for the coordinator-held mode while holding the turn
    Then the command was "devices --solver /x/emfdtd --save --device RTX --no-lock"

  Scenario: Recording devices in lock mode goes through the lock command
    Given a wizard with steps "python", "role" and "enroll" done
    When devices are recorded in lock mode with the lock command "/x/lock nice"
    Then the command was "devices --solver /x/emfdtd --save --device RTX --lock-command /x/lock nice"

  Scenario: Redoing a step closes the steps after it
    Given a wizard with every step done up to "configure"
    When step "enroll" is run again
    Then steps "devices" and "configure" are no longer done

  Scenario: A failed check step reports the fix for each problem
    Given a wizard with every step done up to "configure" and a failing check
    When step "doctor" is run
    Then the step failed with "Start the coordinator"

  Scenario: Refreshing happens only while the window is open and on its interval
    Given a refresher with a 2 second interval
    Then it is not due before it is started
    When it is started
    Then it is due at once and not due again until 2 seconds have passed
    When it is stopped
    Then it is never due

  Scenario: The agent is run as a child process without a shell, with a time limit
    Given a runner for the tool "/p/emfdtd_farm.py" that times out
    When it runs "agent status --json"
    Then the child command was the Python, the tool and the arguments as separate items
    And the result says it did not finish

  Scenario Outline: What autostart installs is named for this operating system
    Then the autostart text for "<system>" mentions "<thing>"

    Examples:
      | system  | thing                  |
      | Darwin  | launchd                |
      | Windows | scheduled task         |
      | Linux   | systemd user unit      |

  Scenario: Unreadable checks and autostart answers are reported
    Then an unreadable doctor answer says it was not understood
    And an unreadable autostart answer says it was not understood
