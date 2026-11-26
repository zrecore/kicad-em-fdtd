Feature: The coordinator can be stopped and started again without losing the queue, and keeps its gates current

  Scenario: Waiting jobs survive a restart and run when a worker arrives
    Given a farm with a coordinator
    And a client "alice"
    When "alice" submits 2 jobs while no worker is connected
    And the coordinator is restarted
    Then both jobs are listed as queued
    When a worker "gpu-box" offering "stub gpu a" connects
    Then both jobs finish

  Scenario: A job that was running when the coordinator stopped is run again with the attempt counted
    Given a farm with a coordinator
    And a client "alice"
    And a worker "gpu-box" offering "stub gpu a"
    When "alice" submits a slow job
    And the job is running on "gpu-box"
    And the coordinator is restarted
    Then the job finishes on "gpu-box" after 2 attempts

  Scenario: A job that has used all its attempts fails when the coordinator restarts
    Given a farm with a coordinator
    And a client "alice"
    And a worker "gpu-box" offering "stub gpu a"
    When "alice" submits a slow job
    And the job is running on "gpu-box"
    And the coordinator is restarted
    And the job is running again after the restart
    And the coordinator is restarted
    Then the job fails with "gave up after 2 attempts" after 2 attempts

  Scenario: Finished jobs and their results survive a restart
    Given a farm with a coordinator
    And a client "alice"
    And a worker "gpu-box" offering "stub gpu a"
    When "alice" submits a job with the setting mesh_preset "medium"
    And the job finishes
    And the coordinator is restarted
    Then "alice" still sees the job as done and can fetch its results

  Scenario: A submission whose input never arrives is dropped
    Given a farm with a coordinator
    And a client "alice"
    When "alice" announces a job but never sends its input
    And the announcement is older than the receive timeout
    Then the job fails with "input never arrived" after 0 attempts

  Scenario: A gate added while the coordinator runs applies to the next job
    Given a farm with a coordinator that re-reads gates.json
    And a client "alice"
    And a worker "gpu-box" offering "stub gpu a" that relies on the coordinator's turn
    When "alice" submits a job with the setting mesh_preset "medium"
    Then the job fails with "holds the GPU turn" after 1 attempt
    When a gate for "gpu-box" is written to gates.json
    And "alice" submits a job with the setting mesh_preset "medium"
    Then the second job finishes
    And the turn was taken and given back for it

  Scenario: A broken gates.json keeps the last good gates
    Given a farm with a coordinator that re-reads gates.json
    And a client "alice"
    And a worker "gpu-box" offering "stub gpu a" that relies on the coordinator's turn
    When a gate for "gpu-box" is written to gates.json
    And "alice" submits a job with the setting mesh_preset "medium"
    And the job finishes
    And gates.json is overwritten with broken JSON
    And "alice" submits a job with the setting mesh_preset "medium"
    Then the second job finishes
    And the turn was taken and given back twice
    And the coordinator is still serving

  Scenario: A damaged jobs file does not stop the coordinator from starting
    Given a farm with a coordinator
    And a client "alice"
    When jobs.json is overwritten with damaged data
    And the coordinator is restarted
    Then the coordinator is still serving

  Scenario: The queue file is private to the user and holds no progress text
    Given a farm with a coordinator
    And a client "alice"
    When "alice" submits 1 jobs while no worker is connected
    Then jobs.json holds that job as queued with its spec and no progress
