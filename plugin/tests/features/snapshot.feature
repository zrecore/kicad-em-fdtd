Feature: Board geometry becomes a snapshot the solver can read
  The plugin converts tracks, arcs and pads to polygons in metres and checks the snapshot before writing it.

  Scenario: A straight track becomes a stadium polygon of the right size
    Given a 10 mm long track of width 0.5 mm
    When it is converted to a polygon
    Then its area is within 0.5 percent of length times width plus a full circle of the track width

  Scenario: An arc through three points is sampled on its circle
    Given an arc from 1,0 through 0,1 to -1,0 in metres
    When it is sampled
    Then every sample lies within 1e-9 of the unit circle and the ends are the given points

  Scenario: A snapshot without a dielectric layer is refused with a clear message
    Given a snapshot whose stackup has only copper
    When it is validated
    Then validation fails naming the stackup

  Scenario: Copper on a layer that is not in the stackup is refused
    Given a snapshot with copper on layer B.Cu but a stackup with only F.Cu
    When it is validated
    Then validation fails naming layer B.Cu

  Scenario: Two pads facing each other across a gap define a port across that gap
    Given pad A spanning x 0 to 1 mm and pad B spanning x 1.3 to 2.3 mm, both spanning y 0 to 0.8 mm
    When a port is made from pad A to pad B
    Then the port runs from x 1.0 to x 1.3 mm at y 0.4 mm and is 0.8 mm wide

  Scenario: Overlapping pads cannot define a port
    Given pad A spanning x 0 to 1 mm and pad B spanning x 0.5 to 1.5 mm, both spanning y 0 to 0.8 mm
    When a port is made from pad A to pad B
    Then port creation fails saying there is no gap

  Scenario: A solver window that is already open is reused
    Given a project folder with a solver window running
    When a new board export is published
    Then no new solver is started and the snapshot file is rewritten

  Scenario: A new solver window is started when none is open
    Given a project folder with no solver window
    When a new board export is published
    Then one solver is started on the new snapshot file

  Scenario: Simulate starts the run in a new window
    Given a project folder with no solver window
    When a run is requested for a new export
    Then the new solver is told to start the run

  Scenario: Simulate asks an open window to run
    Given a project folder with a solver window running
    When a run is requested for a new export
    Then the open window is asked to run through the request file
