# Bench scenes (script_file format: step name, player 1 script, player 2 script; see docs/dev4k/SCRIPTED_INPUT.md)

- `coop_ship.txt`: from launch with the owner's save (a copy in the launcher test profile, tools/dev4k/test_profile.ps1).
  Player 1 presses A every 1.5 s from 24 s to 92 s and pulses the left stick left every 3 s from 67 s; player 2 presses A
  every 1.5 s from 47 s and pulses the left stick right. That walks title -> main menu "Jugar en local" -> lobby (join,
  ready, "Comenzar") -> "Continuar" -> character select (Maki left, Omura right) whatever each screen's load time, then
  stops before gameplay: co-op split screen in the asteroid field (ship: P1 gunner, P2 pilot), nobody moving, from about
  100 s. Measure 120-180 s. button_layout nintendo (accept on the right button).
