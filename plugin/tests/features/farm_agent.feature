Feature: A person configures, checks and inspects the farm agent without an assistant

  Scenario: Configure saves a validated, private file with nothing secret in it
    Given a clean farm home
    When the user configures the agent with the stub solver, the device "stub gpu a" and no lock
    Then the saved configuration is exactly those settings
    And the configuration file can be read only by its owner
    And the configuration holds no key, token or certificate

  Scenario Outline: A bad configuration is refused with a reason
    Given a clean farm home
    When the user configures the agent with <what>
    Then the command fails saying "<message>"
    And nothing is saved

    Examples:
      | what                                    | message                             |
      | no way to take the GPU's turn           | exactly one of                      |
      | two ways to take the GPU's turn         | exactly one of                      |
      | a solver that does not exist            | does not exist                      |
      | no device                               | devices must list                   |
      | a tier that is not one of the tiers     | invalid choice                      |
      | a watchdog of zero seconds              | watchdog must be between            |
      | a lock command that is empty            | lock_command                        |
      | a ceiling of minus five minutes         | max_minutes must be a number        |

  Scenario: The saved configuration can be shown as JSON and replaced from JSON on stdin
    Given a clean farm home
    When the user configures the agent with the stub solver, the device "stub gpu a" and no lock
    And the user runs "agent configure --show --json"
    Then the output is the saved configuration as JSON
    When the user saves this JSON on stdin with "agent configure --json-in --json": a configuration with the device "stub gpu b" and a 45 minute ceiling
    Then the saved configuration has the device "stub gpu b" and a ceiling of 45 minutes
    When the user saves this JSON on stdin with "agent configure --json-in --json": a configuration with an unknown key
    Then the command fails and prints a JSON error naming "unknown setting"

  Scenario: Showing a configuration that does not exist is a clean failure
    Given a clean farm home
    When the user runs "agent configure --show --json"
    Then the command fails and prints a JSON error naming "no saved configuration"

  Scenario: Doctor on a blank machine reports every check with a stable id and a fix
    Given a clean farm home
    When the user runs "agent doctor --json"
    Then the command fails
    And the report has the checks tls13, role, identity, coordinator, certificate, solver, settings, devices, workdir, config in that order
    And every failing check has a title, a detail and a one-line fix

  Scenario: Status of an agent that is not running has the fixed shape
    Given a clean farm home
    When the user runs "agent status --json"
    Then the status says not running and has exactly the keys running, pid, started, uptime_s, connected, coordinator, job, device, last_error and log

  Scenario: Logs prints the last lines
    Given a clean farm home
    And an agent log of 30 lines
    When the user runs "agent logs -n 5"
    Then the output is the last 5 lines of the log

  Scenario: Autostart is off by default and reports how it would be done
    Given a clean farm home
    When the user runs "agent autostart status --json"
    Then autostart is off and names a mechanism
    When the user runs "agent autostart on"
    Then the command fails saying "configure the agent first"

  Scenario Outline: The autostart files and commands are exactly what is documented
    Given a clean farm home
    Then the autostart for <platform> uses the mechanism <mechanism> and the text or command in <what>

    Examples:
      | platform | mechanism | what          |
      | darwin   | launchd   | the plist     |
      | linux    | systemd   | the unit      |
      | win32    | schtasks  | the task argv |

  Scenario Outline: Turning autostart on and off installs and removes only its own entry
    Given a clean farm home
    Then on <platform> autostart on writes its file, runs the install commands, and off removes them again

    Examples:
      | platform |
      | darwin   |
      | linux    |
      | win32    |
