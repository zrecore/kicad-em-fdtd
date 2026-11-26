Feature: A job is not sent to a worker whose time ceiling it would outrun

  Background:
    Given a scheduler with a worker "win" at 1600 Mcells/s that allows 45 minutes
    And the worker "mac" offers a device at 1200 Mcells/s and allows 120 minutes

  Scenario: A long job skips the fast worker whose ceiling is too low
    When "client:alice" submits a job that needs about 49 minutes on the fast device and its input arrives
    And offers are made
    Then the job is running on "mac"

  Scenario: A short job still goes to the fastest worker
    When "client:alice" submits a job that needs about 10 minutes on the fast device and its input arrives
    And offers are made
    Then the job is running on "win"

  Scenario: A job with no estimate behaves as before
    When "client:alice" submits a job with no estimate and its input arrives
    And offers are made
    Then the job is running on "win"

  Scenario: The job waits, and says why, when the worker that fits is busy
    When "client:alice" submits a job pinned to "mac" and its input arrives
    And "client:alice" submits a job that needs about 49 minutes on the fast device and its input arrives
    And offers are made
    Then the job that needs about 49 minutes is queued with the reason "waiting for mac (busy)"

  Scenario: Every online ceiling is too low: the job stays queued with the numbers, then runs when a longer worker connects
    Given the worker "mac" is gone
    When "client:alice" submits a job that needs about 49 minutes on the fast device and its input arrives
    And offers are made
    Then the job is queued with the reason "needs about 49 min; the online workers allow 45 (win)"
    When a worker "big" that allows 90 minutes connects with a device at 1000 Mcells/s
    And offers are made
    Then the job is running on "big"

  Scenario: No worker online at all
    Given the worker "mac" is gone
    And the worker "win" is gone
    When "client:alice" submits a job that needs about 10 minutes on the fast device and its input arrives
    And offers are made
    Then the job is queued with the reason "no worker is online"

  Scenario: A device with no measured speed is judged at the slowest measured speed
    Given the worker "new" offers a device with no measured speed and allows 40 minutes
    And the worker "win" is gone
    When "client:alice" submits a job pinned to "mac" and its input arrives
    And "client:alice" submits a job that needs about 20 minutes on the fast device and its input arrives
    And offers are made
    Then the job is running on "new"

  Scenario: A worker that is not telling its ceiling is learned from a time-limit failure
    Given the worker "old" offers a device at 1600 Mcells/s and tells no ceiling
    And the worker "win" is gone
    And the worker "mac" is gone
    When "client:alice" submits a job with no estimate and its input arrives
    And offers are made
    And the job fails with "over the time limit of 30.0 minutes"
    Then the job's reason is "over the time limit of 30.0 minutes on old (its ceiling). Raise the ceiling on that machine (emfdtd-farm agent configure --max-minutes N) or send the job to a faster/longer-allowed worker with --device."
    When "client:alice" submits a job that needs about 49 minutes on the fast device and its input arrives
    And offers are made
    Then the job is queued with the reason "needs about 49 min; the online workers allow 30 (old)"

  Scenario: A pinned job goes only to that worker, even when another is faster
    When "client:alice" submits a job pinned to "mac" and its input arrives
    And offers are made
    Then the job is running on "mac"

  Scenario: A pinned job waits for its worker
    When "client:alice" submits a job pinned to "mac" and its input arrives
    And "client:alice" submits a job pinned to "mac" and its input arrives
    And offers are made
    Then the second job is queued with the reason "waiting for mac (busy)"

  Scenario: A pin to a worker that is not connected
    Given the worker "mac" is gone
    When "client:alice" submits a job pinned to "mac" and its input arrives
    And offers are made
    Then the job is queued with the reason "waiting for mac (not connected)"

  Scenario: A pin can name the device as well
    When "client:alice" submits a job pinned to "mac:Apple" and its input arrives
    And offers are made
    Then the job is running on "mac"
    When "client:alice" submits a job pinned to "mac:Nonexistent" and its input arrives
    And offers are made
    Then the job is queued with the reason "mac has no device matching 'Nonexistent'"

  Scenario: A pinned job that cannot fit says so
    When "client:alice" submits a job pinned to "win" that needs about 49 minutes on the fast device and its input arrives
    And offers are made
    Then the job is queued with the reason "needs about 49 min but win allows 45"

  Scenario Outline: A bad estimate or pin is refused
    Then the job spec <spec> is refused

    Examples:
      | spec                                         |
      | {"estimate": {"cells": -5, "steps": 10}}     |
      | {"estimate": {"cells": 1e5}}                 |
      | {"estimate": {"cells": "many", "steps": 1}}  |
      | {"estimate": {"cells": 1e13, "steps": 1}}    |
      | {"estimate": [1, 2]}                         |
      | {"pin": "--rm"}                              |
      | {"pin": "a b:c;d"}                           |
      | {"pin": 7}                                   |

  Scenario: The scheduling hints never reach the solver's worker
    Then the worker's copy of a job spec with an estimate and a pin has neither
