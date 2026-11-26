Feature: A new machine becomes a farm worker through one guided command

  Scenario: Setup enrols, records the GPU, configures, checks and starts the agent without asking
    Given a farm with a coordinator
    And an open invitation for a worker "setup-box"
    When a new machine runs setup with every answer as a flag
    Then setup ends successfully and says the checks passed
    And the coordinator knows the worker "setup-box" and sees it online
    And that machine's agent status says running and connected
    When that machine stops its agent
    Then that machine's agent process is gone

  Scenario: Setup with --yes never asks and says which flag is missing
    Given a farm with a coordinator
    When a new machine runs setup with --yes and no other flags
    Then setup fails saying "missing value"

  Scenario: Setup asks for what is missing when it is not given --yes
    Given a farm with a coordinator
    And an open invitation for a worker "asked-box"
    When a new machine runs setup and types its answers
    Then setup ends successfully and says the checks passed
    And the coordinator knows the worker "asked-box" and sees it online
    And that machine's agent status says running and connected

  Scenario: Setup refuses a Python without TLS 1.3 before changing anything
    Given a clean farm home
    When setup is run on a Python that has no TLS 1.3
    Then setup fails saying "TLS 1.3"
    And the worker role is still off
