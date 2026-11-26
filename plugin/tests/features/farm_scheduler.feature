Feature: The scheduler shares devices fairly and never loses or doubles a job

  Background:
    Given a scheduler with a worker "win" offering one RTX at 1200 Mcells/s

  Scenario: A queued job goes to an idle device and the device is then busy
    When "client:alice" submits a job and its input arrives
    And offers are made
    Then the job is running on "win" and the device is busy

  Scenario: A job whose input has not arrived is not started
    When "client:alice" submits a job
    And offers are made
    Then no job is running

  Scenario: Jobs wait when every device is busy and start when one frees up
    When "client:alice" submits 2 jobs and their inputs arrive
    And offers are made
    Then 1 job is running and 1 is queued
    When the running job finishes
    And offers are made
    Then 0 jobs are queued

  Scenario: A user with many jobs does not starve another user
    When "client:alice" submits 3 jobs and their inputs arrive
    And "client:bob" submits 1 job and its input arrives
    And the first offer runs and finishes 1 job
    And offers are made
    Then the running job belongs to "client:bob"

  Scenario: The fastest idle device is chosen
    Given the worker "mac" offers a second device at 1800 Mcells/s
    When "client:alice" submits a job and its input arrives
    And offers are made
    Then the job is running on "mac" and the device is busy

  Scenario: A device with no measured speed ranks after measured ones
    Given the worker "mac" offers a second device at 0 Mcells/s
    When "client:alice" submits a job and its input arrives
    And offers are made
    Then the job is running on "win" and the device is busy

  Scenario: A device with no measured speed still gets a job when it is the only one idle
    Given the worker "mac" offers a second device at 0 Mcells/s
    When "client:alice" submits 2 jobs and their inputs arrive
    And offers are made
    Then both devices are busy

  Scenario: Equal speeds are broken by worker name
    Given the worker "aaa" offers a second device at 1200 Mcells/s
    When "client:alice" submits a job and its input arrives
    And offers are made
    Then the job is running on "aaa" and the device is busy

  Scenario: A lost worker's job goes back to the queue and a late result is refused
    When "client:alice" submits a job and its input arrives
    And offers are made
    And the worker "win" is lost
    Then the job is queued with 1 attempt
    And a result for the old lease is refused

  Scenario: A job that keeps losing its worker fails after the attempt limit
    When "client:alice" submits a job and its input arrives
    And offers are made
    And the worker "win" is lost
    And the worker "win" comes back and offers are made
    And the worker "win" is lost
    Then the job has failed and says it gave up

  Scenario: A silent worker's lease expires
    When "client:alice" submits a job and its input arrives
    And offers are made
    And 100 seconds pass without a heartbeat
    Then the job is queued with 1 attempt

  Scenario: Heartbeats keep a lease alive
    When "client:alice" submits a job and its input arrives
    And offers are made
    And a heartbeat arrives every 20 seconds for 100 seconds
    Then the job is running on "win" and the device is busy

  Scenario: Only the submitter can cancel
    When "client:alice" submits a job and its input arrives
    Then "client:bob" cannot cancel it
    And "client:alice" can cancel it

  Scenario: A user sees only their own jobs
    When "client:alice" submits a job and its input arrives
    And "client:bob" submits a job and its input arrives
    Then "client:alice" sees 1 job and the admin view shows 2

  Scenario: A user cannot queue without limit
    When "client:alice" submits jobs until the limit of 3 waiting jobs is reached
    Then the next submission is refused

  Scenario: A device that reports itself unhealthy gets no jobs
    When the device reports not ok
    And "client:alice" submits a job and its input arrives
    And offers are made
    Then no job is running

  Scenario: A job failure that should not be retried is final
    When "client:alice" submits a job and its input arrives
    And offers are made
    And the worker reports a fatal failure
    Then the job has failed and the device is idle

  Scenario: Jobs of one sweep stay on one kind of device
    Given the worker "mac" offers two devices of one kind at 1800 Mcells/s
    When "client:alice" submits 2 jobs of one sweep and their inputs arrive
    And offers are made
    Then both jobs run on "mac"

  Scenario: Finished jobs are forgotten after the retention time
    When "client:alice" submits a job and its input arrives
    And offers are made
    And the running job finishes
    And 8 days pass
    Then forgetting jobs older than 7 days removes it

  Scenario: The solver's progress line becomes a percentage on the job
    When "client:alice" submits a job and its input arrives
    And offers are made
    And the solver reports "progress: 41200 steps, about 37%, 312 s"
    Then the job shows 37 percent
    When the solver reports "starting the solver"
    Then the job still shows 37 percent

  Scenario: The queue survives a save and a restore as plain data
    When "client:alice" submits a job and its input arrives
    And "client:alice" submits a job
    And offers are made
    And the scheduler is saved as JSON and restored into a new one
    Then the new scheduler lists 2 jobs
    And the job that was running is queued again with 1 attempt and the reason "the coordinator restarted"
    And the job whose input had not arrived is still receiving

  Scenario: A job that was running with no attempts left fails when restored
    When "client:alice" submits a job and its input arrives
    And offers are made
    And the worker "win" is lost
    And the worker "win" comes back and offers are made
    And the scheduler is saved as JSON and restored into a new one
    Then the restored job has failed and says it gave up

  Scenario: A damaged saved row is skipped and the others are kept
    When "client:alice" submits a job and its input arrives
    And the scheduler is saved with one extra damaged row and restored into a new one
    Then the new scheduler lists 1 jobs

  Scenario: Progress text and percentages are not kept across a restart
    When "client:alice" submits a job and its input arrives
    And offers are made
    And the solver reports "progress: 41200 steps, about 37%, 312 s"
    And the scheduler is saved as JSON and restored into a new one
    Then the restored job shows no progress and no percentage

  Scenario: A submission whose input never arrives fails after the receive timeout
    When "client:alice" submits a job
    And 7200 seconds pass without a heartbeat
    Then the job has failed with the reason "the input never arrived"
