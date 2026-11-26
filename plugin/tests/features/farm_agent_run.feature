Feature: The agent starts and stops a real worker and says what it is doing

  Background:
    Given a farm with a coordinator
    And a client "alice"
    And this machine is enrolled as the worker "agent-box" and the agent is configured for the stub GPU

  Scenario: Doctor passes against a live coordinator
    When the user runs "agent doctor --json"
    Then every check passes
    And the certificate check says the coordinator accepted us

  Scenario: A certificate from another farm is rejected by the doctor
    Given the worker identity is replaced by one issued by a different farm
    When the user runs "agent doctor --json"
    Then the command fails
    And the check "certificate" fails saying "rejected"

  Scenario: Start runs a worker that connects, and status says so
    When the user starts the agent
    Then the coordinator sees the worker "agent-box" online
    And the agent status says running and connected to the coordinator with a pid and an uptime

  Scenario: A job runs on the agent's worker and the status shows it
    When the user starts the agent
    And "alice" submits a slow job
    Then the agent status shows a job with a phase
    And the job finishes

  Scenario: A second start is refused
    When the user starts the agent
    And the user starts the agent again
    Then the second start fails saying "already running"

  Scenario: A stale pid file of a dead agent is cleaned up
    Given a pid file naming a process that is gone
    When the user starts the agent
    Then the agent status says running and connected to the coordinator with a pid and an uptime

  Scenario: A recycled pid is never mistaken for the agent
    Given a pid file naming an unrelated running process
    Then the agent status says not running
    When the user stops the agent
    Then the unrelated process is still running
    When the user starts the agent
    Then the agent status says running and connected to the coordinator with a pid and an uptime

  Scenario: Start refuses, with the reason, when a check fails
    Given the worker role is turned off
    When the user starts the agent
    Then the start fails saying "The worker role is on"
    And no agent process was started

  Scenario: Stop ends the agent and the solver it is running
    When the user starts the agent
    And "alice" submits a job whose solver hangs
    And the solver is running
    And the user stops the agent
    Then the agent process is gone
    And the solver process is gone
    And the agent status says not running

  Scenario: Restart gives a new agent that connects
    When the user starts the agent
    And the user restarts the agent
    Then the agent status says running and connected to the coordinator with a pid and an uptime
    And the pid changed

  Scenario: The log records what the agent did
    When the user starts the agent
    Then the log mentions that the worker is connecting out

  Scenario Outline: Doctor names each problem and its one-line fix
    Given <problem>
    When the user runs "agent doctor --json"
    Then the command fails
    And the check "<id>" fails saying "<text>"
    And that check has a fix

    Examples:
      | problem                                          | id          | text                    |
      | the worker role is turned off                    | role        | off                     |
      | no worker identity is stored                     | identity    | no worker identity      |
      | no coordinator address is stored                 | coordinator | no coordinator address  |
      | the coordinator is stopped                       | coordinator | ConnectionRefused       |
      | the recorded devices are deleted                 | devices     | no recorded devices     |
      | the configured device matches nothing            | devices     | none of                 |
      | the work folder has almost no free disk          | workdir     | GiB free                |
      | this Python has no TLS 1.3                       | tls13       | no TLS 1.3              |

  Scenario: Start, stop and status have fixed JSON shapes
    When the user runs "agent start --json"
    Then the output is a status with the fixed keys and running true
    When the user runs "agent stop --json"
    Then the output has exactly the keys was_running, stopped and forced and says it stopped a running agent
    When the user runs "agent stop --json"
    Then the output says there was no running agent

  Scenario: A frozen agent is killed with its solver when it ignores the polite stop
    When the user starts the agent
    And "alice" submits a job whose solver hangs
    And the solver is running
    And the agent is frozen so that it cannot answer a polite stop
    And the user stops the agent with 2 seconds of patience
    Then the stop had to kill it
    And the agent process is gone
    And the solver process is gone
