Feature: The coordinator machine can offer its own GPU to the farm, once, on request

  Scenario: Nothing is offered until the user asks
    Given a farm with a coordinator
    And the coordinator role is on and this machine knows its coordinator
    Then the local GPU status says available, not enabled, no worker name

  Scenario: The coordinator role must be on first
    Given a farm with a coordinator
    Then the local GPU status says it is unavailable because "turn on the coordinator role first"
    And enabling it fails saying "turn on the coordinator role first"

  Scenario: Enabling enrolls this machine, picks the usable GPUs and saves the agent settings
    Given a farm with a coordinator
    And the coordinator role is on and this machine knows its coordinator
    When the user enables the local GPU with the stub solver without starting the agent
    Then the worker role is on
    And this machine is a member of the farm as a worker that may run jobs
    And the saved agent settings offer the usable GPUs "Stub GPU A, Stub GPU B" and the stub solver
    And the local GPU status says enabled with the same worker name

  Scenario: Enabling twice keeps one identity
    Given a farm with a coordinator
    And the coordinator role is on and this machine knows its coordinator
    When the user enables the local GPU with the stub solver without starting the agent
    And the user enables the local GPU with the stub solver without starting the agent
    Then the farm has exactly one worker identity for this machine

  Scenario: Disabling turns the worker role off and keeps the identity
    Given a farm with a coordinator
    And the coordinator role is on and this machine knows its coordinator
    When the user enables the local GPU with the stub solver without starting the agent
    And the user disables the local GPU
    Then the worker role is off
    And the local GPU status says not enabled but keeps the worker name

  Scenario: The confirmation must be typed
    Given a farm with a coordinator
    And the coordinator role is on and this machine knows its coordinator
    When the user runs local-worker enable and types "no"
    Then nothing is enabled
