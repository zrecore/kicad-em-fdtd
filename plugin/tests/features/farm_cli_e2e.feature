Feature: A person sets up a farm with the command line alone

  Scenario: Three machines, one coordinator, one worker, one client, one sweep
    Given three machines with their own farm folders
    When the coordinator machine enables its role, initialises and opens an invitation for a worker
    And the worker machine enrols with that code and fingerprint
    And the coordinator opens an invitation for a client and the client machine enrols
    And the coordinator serves and the worker connects out
    And the client submits a sweep of 2 values and waits for the results
    Then the client has the results of both jobs
    And the coordinator lists the two enrolled devices
    And revoking the worker removes it from the farm

  Scenario: Stopping the coordinator with SIGTERM gives back a GPU turn it is holding
    Given three machines with their own farm folders
    When the coordinator machine enables its role, initialises and opens an invitation for a worker
    And the worker machine enrols with that code and fingerprint
    And the coordinator opens an invitation for a client and the client machine enrols
    And the coordinator serves with a gate for the worker and the worker relies on the coordinator's turn
    And the client submits a job whose solver hangs
    And the coordinator is holding the turn for the running job
    And the coordinator is sent SIGTERM
    Then the coordinator exits cleanly within 10 seconds
    And the turn was released and its hold command ended
