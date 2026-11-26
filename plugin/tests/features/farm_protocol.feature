Feature: Farm frames and messages are validated before anything acts on them

  Scenario: A valid message survives a round trip
    When a heartbeat message is framed and read back
    Then the same message comes out

  Scenario Outline: A malformed frame is refused
    When a peer sends <bad>
    Then the reader raises a protocol error

    Examples:
      | bad                         |
      | a frame longer than the cap |
      | a zero-length frame         |
      | a frame that is not JSON    |
      | a JSON array                |
      | an unknown message type     |
      | a message missing a field   |
      | a number given as true      |
      | a truncated frame body      |

  Scenario: A clean end of stream is not an error
    When a peer closes without sending anything
    Then the reader returns nothing
