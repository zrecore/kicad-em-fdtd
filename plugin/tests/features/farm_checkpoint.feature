Feature: A job that reaches its time ceiling pauses at a checkpoint and can be resumed

  Background:
    Given a farm with a coordinator
    And a client "alice"
    And a client "bob"
    And a worker "w1" offering "stub gpu a"
    And the worker "w1" has a time limit of 0.04 minutes

  Scenario: A job at the time ceiling pauses instead of failing
    When "alice" submits a long job that writes checkpoints
    Then the job is paused with a checkpoint step
    And the reason says "(ceiling reached)" and how to resume it
    And the worker still holds the checkpoint folder

  Scenario: Resume continues from the checkpoint and the job finishes
    When "alice" submits a job that needs two runs
    And the job is paused with a checkpoint step
    And "alice" resumes it
    Then the job finishes
    And the summary says the run was resumed at the checkpoint step

  Scenario: Only the owner may resume a paused job
    When "alice" submits a long job that writes checkpoints
    And the job is paused with a checkpoint step
    Then "bob" is refused when resuming it with "only the submitter"

  Scenario: Resume may shorten the ceiling but never raise it above the worker's own
    When "alice" submits a long job that writes checkpoints
    And the job is paused with a checkpoint step
    Then resuming it with a ceiling of 5 minutes is refused with "allows at most 0.04 minutes per run"

  Scenario: Restart drops the checkpoint and runs from step 0
    When "alice" submits a job that needs two runs
    And the job is paused with a checkpoint step
    And "alice" restarts it
    Then the coordinator has dropped the job's checkpoint

  Scenario: Cancelling a paused job removes its checkpoint from the worker
    When "alice" submits a long job that writes checkpoints
    And the job is paused with a checkpoint step
    And "alice" cancels it
    Then the worker no longer holds the checkpoint folder

  Scenario: A checkpoint the solver refuses makes the job start again from step 0
    When "alice" submits a job that needs two runs
    And the job is paused with a checkpoint step
    And the worker "w1" has no time limit
    And the checkpoint files on the worker are damaged
    And "alice" resumes it
    Then the job finishes
    And the summary does not say the run was resumed

  Scenario: The worker lists the checkpoints it holds and forgets them after the retention time
    When "alice" submits a long job that writes checkpoints
    And the job is paused with a checkpoint step
    Then the worker reports one held checkpoint for the job
    And the worker removes it when it is older than seven days

  Scenario: An old worker without checkpoint support keeps the old behavior
    Given the worker "w1" is replaced by one without checkpoint support
    When "alice" submits a long job that writes checkpoints
    Then the job fails saying "over the time limit"

  Scenario: A worker that stops politely pauses its job and resumes it when it is back
    Given the worker "w1" has no time limit
    When "alice" submits a job that runs for a few seconds and writes checkpoints
    And the job has written its first checkpoint
    And the worker "w1" stops politely
    Then the job waits for "w1" and says it holds the checkpoint
    When the worker "w1" connects again
    Then the job finishes
    And the summary says the run was resumed at the checkpoint step

  Scenario: A job whose worker was lost waits for it instead of starting elsewhere
    Given the worker "w1" has no time limit
    When "alice" submits a job that runs for a few seconds and writes checkpoints
    And the job has written its first checkpoint
    And the worker "w1" disappears
    And a worker "w2" offering "stub gpu a" connects
    Then the job stays waiting for "w1" while "w2" is idle
    When the worker "w1" connects again
    Then the job finishes
    And the summary says the run was resumed at the checkpoint step

  Scenario: After the waiting time the job starts from step 0 on any worker
    Given the worker "w1" has no time limit
    And the coordinator waits only 2 seconds for a worker's checkpoint
    When "alice" submits a job that runs for a few seconds and writes checkpoints
    And the job has written its first checkpoint
    And the worker "w1" disappears
    And a worker "w2" offering "stub gpu a" connects
    Then the job finishes on "w2"
    And the summary does not say the run was resumed
