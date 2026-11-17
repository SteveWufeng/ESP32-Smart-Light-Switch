#!/usr/bin/env bash
set -u

HOST="${SERVO_HOST:-10.164.78.2}"
PORT="${SERVO_PORT:-80}"
USER_NAME="${SERVO_USER:-hac}"
PASSWORD="${SERVO_PASS:-}"
INTERVAL="${SERVO_INTERVAL:-1.0}"

if [[ -z "$PASSWORD" ]]; then
  read -rsp "Password for ${USER_NAME}@${HOST}: " PASSWORD
  echo
fi

URL="http://${HOST}:${PORT}/press"
count=0

trap 'printf "\nStopped after %d press(es).\n" "$count"; exit 0' INT TERM

printf 'Pressing %s every %ss (Ctrl+C to stop)\n' "$URL" "$INTERVAL"

while true; do
  response=$(curl -sS --max-time 5 -u "${USER_NAME}:${PASSWORD}" -w $'\n%{http_code}' "$URL" 2>&1) || true
  code="${response##*$'\n'}"
  body="${response%$'\n'*}"
  count=$((count + 1))
  printf '[%d] HTTP %s %s\n' "$count" "$code" "$body"
  sleep "$INTERVAL"
done
