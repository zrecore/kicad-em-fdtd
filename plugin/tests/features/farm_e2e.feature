Feature: Jobs travel from a client through the coordinator to a worker's GPU and back

  Background:
    Given a farm with a coordinator
    And a worker "gpu-box" offering "stub gpu a"
    And a client "alice"

  Scenario: A job runs on the worker and its results come back
    When "alice" submits a job with the setting mesh_preset "medium"
    Then the job finishes
    And the results hold s11.s1p, summary.txt and settings.json and the run log
    And the summary names the worker's device and the settings file shows mesh_preset=medium
    And the worker's job folder is gone

  Scenario: Two devices run two jobs at once
    Given a worker "second-box" offering "stub gpu b"
    When "alice" submits 2 slow jobs
    Then both jobs are running at the same time on different workers

  Scenario: A forbidden setting is refused at submission
    When "alice" submits a job with the setting backend "cpu"
    Then the submission is refused

  Scenario: A job the solver rejects fails without a retry
    When "alice" submits a job the solver rejects
    Then the job fails with "exited with 7" after 1 attempt

  Scenario: A crashing solver fails the job with its exit code
    When "alice" submits a job whose solver crashes
    Then the job fails with "exited with 3" after 1 attempt

  Scenario: A job that outruns the time limit is killed
    Given the worker "gpu-box" has a time limit of 0.03 minutes
    When "alice" submits a job whose solver hangs
    Then the job fails with "time limit" after 1 attempt
    And the solver process is gone

  Scenario: Losing the worker mid-job requeues it and another worker finishes it
    When "alice" submits a slow job
    And the job is running on "gpu-box"
    And the worker "gpu-box" disappears
    And a worker "spare-box" offering "stub gpu b" connects
    Then the job finishes on "spare-box" after 2 attempts

  Scenario: Cancelling a running job stops the solver
    When "alice" submits a job whose solver hangs
    And the job is running on "gpu-box"
    And "alice" cancels it
    Then the job is cancelled and the solver process is gone

  Scenario: Another user cannot see, cancel or fetch someone else's job
    Given a client "bob"
    When "alice" submits a job with the setting mesh_preset "medium"
    And the job finishes
    Then "bob" sees no jobs
    And "bob" cannot cancel it or fetch its results

  Scenario: A worker cannot submit and a client cannot act as a worker
    Then the worker "gpu-box" cannot submit a job
    And the client "alice" cannot connect as a worker

  Scenario: A revoked worker is dropped and gets no more jobs
    When the worker "gpu-box" certificate is revoked
    Then the coordinator shows the worker "gpu-box" offline
    And "alice" submitting a job leaves it queued

  Scenario: An input over the client's size limit is refused
    Given the client "tiny" is limited to 5 bytes of input
    Then "tiny" cannot submit a normal snapshot

  Scenario: A job with an estimate and a pin runs on the pinned worker
    When "alice" submits a job pinned to "gpu-box" with an estimate
    Then the job finishes

  Scenario: A pin to a worker that was never enrolled is refused
    When "alice" submits a job pinned to "nobody"
    Then the submission is refused saying "no enrolled worker named 'nobody'"

  Scenario: A worker tells the coordinator its own ceiling
    Given the worker "short-box" has its own ceiling of 7 minutes
    Then the coordinator knows the ceiling of "short-box" is 7 minutes
