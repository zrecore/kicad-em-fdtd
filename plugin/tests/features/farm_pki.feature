Feature: Devices are identified by certificates from the farm's own CA

  Scenario: An enrolled worker connects and the coordinator sees who it is
    Given a farm with a coordinator and a worker named "win4060"
    When the worker connects to the coordinator
    Then the coordinator sees the identity "worker" "win4060"
    And the connection used TLS 1.3

  Scenario: A certificate from another farm is refused
    Given a farm with a coordinator and a worker named "win4060"
    And a worker certificate issued by a different farm
    When the worker connects to the coordinator
    Then the connection fails

  Scenario: A client with no certificate is refused
    Given a farm with a coordinator and a worker named "win4060"
    When a client with no certificate connects
    Then the connection fails

  Scenario: A revoked certificate is no identity
    Given a farm with a coordinator and a worker named "win4060"
    And the worker's certificate is revoked
    When the worker connects to the coordinator
    Then the coordinator sees no identity

  Scenario: The CA is never overwritten
    Given a farm with a coordinator and a worker named "win4060"
    When a second CA is initialised
    Then it is refused

  Scenario: Fingerprints print in groups and parse back
    Given a farm with a coordinator and a worker named "win4060"
    Then the coordinator fingerprint survives printing and parsing
    And a short or non-hex fingerprint is refused

  Scenario Outline: Bad device names are refused
    Given a farm with a coordinator and a worker named "win4060"
    When a certificate is requested for the name "<name>"
    Then it is refused

    Examples:
      | name       |
      | a b        |
      | x/y        |
      | ../escape  |

  Scenario: A Python without TLS 1.3 cannot run the farm
    Given a farm with a coordinator and a worker named "win4060"
    When the Python has no TLS 1.3
    Then it is refused
