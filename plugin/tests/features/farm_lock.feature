Feature: The farm hands out GPU turns itself
  The coordinator has its own turn manager, so two GPUs of one machine can take separate turns and no
  outside lock script is needed.

  Scenario: Turns of one lock are given in the order they were asked for
    Given a lock manager
    When "first" asks for the lock "gpu" and then "second" does
    Then "first" holds the turn and "second" waits
    When "first" gives the turn back
    Then "second" holds the turn

  Scenario: Two different locks never wait for each other
    Given a lock manager
    When "first" asks for the lock "rtx" and then "second" asks for the lock "radeon"
    Then both hold their turn at once

  Scenario: A turn whose time is up ends by itself and the next in line gets it
    Given a lock manager
    When "first" asks for the lock "gpu" for 0.2 seconds and then "second" does
    Then the turn of "first" is lost and "second" holds the turn

  Scenario: Someone who stops waiting leaves the queue
    Given a lock manager
    When "first" asks for the lock "gpu" and then "second" does and "second" gives up
    And "third" asks for the lock "gpu" and "first" gives the turn back
    Then "third" holds the turn

  Scenario: A gate for one GPU of a worker wins over the gate of the whole worker
    Given a farm with a coordinator
    And a worker "gpu-box" offering "stub gpu a"
    And gates for "gpu-box" and for its GPU "stub gpu a"
    Then the gate of that GPU is the specific one and any other device gets the worker's gate

  Scenario: A job takes the farm's own turn before it runs and gives it back after
    Given a farm with a coordinator
    And a client "alice"
    And the coordinator keeps the turn of "gpu-box" in its own lock "box"
    And a worker "gpu-box" offering "stub gpu a" that relies on the coordinator's turn
    When "alice" submits a job with the setting mesh_preset "medium"
    And the job finishes
    Then the own lock "box" was held during the job and is free again

  Scenario: A gates file needs either a lock name or hold and release commands
    Given a farm with a coordinator
    Then a gate with a lock name is accepted and a gate with both or neither is refused

  Scenario: Enrolled machines queue for a CPU or GPU lock over the network, first come first served
    Given a farm with a coordinator
    And a client "alice"
    And a client "bob"
    When "alice" takes the lock "cpu:mac" and "bob" asks for it
    Then "bob" is told one turn is ahead and waits
    When "alice" hangs up
    Then "bob" is granted the turn
    And the farm status lists the lock "cpu:mac" held by "bob"

  Scenario: A lock name that is not cpu: or gpu: is refused
    Given a farm with a coordinator
    And a client "alice"
    Then asking for the lock "anything" is refused

  Scenario: A command run inside a lock turn gives the turn back when it ends
    Given a farm with a coordinator
    And a client "alice"
    When "alice" runs a command inside the lock "cpu:mac"
    Then the command ran, its exit code came back and the lock is free
