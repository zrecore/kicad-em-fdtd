Feature: A worker never uses the GPU outside a turn, and keeps to its own limits

  Scenario: A worker refuses to start without a way to take its GPU turn
    Then constructing a worker with no turn mechanism fails
    And constructing a worker with two turn mechanisms fails

  Scenario: The coordinator can lower a worker's limits but never raise them
    Then offered limits are clamped to the worker's own ceilings

  Scenario: Every Vulkan call goes through the lock command and the heartbeat makes none
    Given a farm with a coordinator
    And a client "alice"
    And a worker "gpu-box" offering "stub gpu a" that wraps every GPU use in a lock command
    When the worker has stayed connected through several heartbeats
    Then the lock command was used once, to list devices
    When "alice" submits a job with the setting mesh_preset "medium"
    And the job finishes
    Then the lock command also wrapped a device check and the solve
    And it never wrapped the settings listing and the solver was never asked for a dry run

  Scenario: A coordinator-held turn is taken before the offer and released after the job
    Given a farm with a coordinator
    And a client "alice"
    And the coordinator holds the turn for the worker "gpu-box"
    And a worker "gpu-box" offering "stub gpu a" that relies on the coordinator's turn
    When "alice" submits a job with the setting mesh_preset "medium"
    And the job finishes
    Then the turn was held before the job and released after it

  Scenario: A worker that relies on the coordinator's turn refuses an offer that carries none
    Given a farm with a coordinator
    And a client "alice"
    And a worker "gpu-box" offering "stub gpu a" that relies on the coordinator's turn
    When "alice" submits a job with the setting mesh_preset "medium"
    Then the job fails with "holds the GPU turn" after 1 attempt

  Scenario: Cancelling a job gives the turn back
    Given a farm with a coordinator
    And a client "alice"
    And the coordinator holds the turn for the worker "gpu-box"
    And a worker "gpu-box" offering "stub gpu a" that relies on the coordinator's turn
    When "alice" submits a job whose solver hangs
    And the job is running on "gpu-box"
    And "alice" cancels it
    Then the job is cancelled and the solver process is gone
    And the turn is released

  Scenario: Private files are restricted to the current user on Windows
    Then restricting a file on Windows removes inheritance and grants only the current user

  Scenario: A silent network partition stops the solver and the turn is not given back before it is dead
    Given a farm whose coordinator pings every 0.2 seconds with a 0.5 second turn margin
    And a client "alice"
    And the coordinator holds the turn for the worker "gpu-box"
    And a worker "gpu-box" offering "stub gpu a" that relies on the coordinator's turn, reaches it through a relay and has a 1.5 second watchdog
    When "alice" submits a job whose solver hangs
    And the job is running on "gpu-box"
    And the network between them goes silent
    Then the solver is killed within 4 seconds
    And the turn is given back only after the solver was dead

  Scenario: A hold command that ends while the job runs stops the job
    Given a farm with a coordinator
    And a client "alice"
    And the coordinator holds the turn for the worker "gpu-box" but its hold command ends after 1.5 seconds
    And a worker "gpu-box" offering "stub gpu a" that relies on the coordinator's turn
    When "alice" submits a job whose solver hangs
    And the job is running on "gpu-box"
    Then the solver is killed within 6 seconds
    And the audit log says the turn ended early

  Scenario: The gate can cap the job's time and the job limit follows
    Given a farm with a coordinator
    And a client "alice"
    And the coordinator holds the turn for the worker "gpu-box" with a cap of 0.03 minutes
    And a worker "gpu-box" offering "stub gpu a" that relies on the coordinator's turn
    When "alice" submits a job whose solver hangs
    Then the job fails with "time limit" after 1 attempt
    And the turn is released

  Scenario: A cancel while the worker waits for its lock command stops it and the solve never starts
    Given a farm with a coordinator
    And a client "alice"
    And a worker "gpu-box" offering "stub gpu a" whose lock command makes it wait 8 seconds for the turn
    When "alice" submits a job that would write a solver pid file
    And the worker is waiting for its lock command
    And "alice" cancels it
    Then the waiting lock command is gone within 3 seconds
    And the solver never starts
    And the job is cancelled
