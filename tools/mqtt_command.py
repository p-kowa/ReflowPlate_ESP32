import argparse
import getpass
import re
import sys


def command(value):
    if (value in ("stop", "table", "table reset") or re.fullmatch(r"\d+", value)
            or re.fullmatch(r"pulse \d+", value) or re.fullmatch(r"del \d+", value)
            or re.fullmatch(r"set \d+ (off|hyst|lag) \d+(\.\d+)?", value)):
        return value
    raise argparse.ArgumentTypeError('use "stop", a target such as "50", "pulse 5", "set 160 lag 30", "del 160" or "table"')


def main():
    parser = argparse.ArgumentParser(description="Send a command to the ReflowPlate over MQTT.")
    parser.add_argument("host", help="MQTT broker hostname or IP address")
    parser.add_argument("command", type=command, help='"stop", target (e.g. "50"), "pulse 5", "set 160 lag 30", "del 160", "table" or "table reset"')
    parser.add_argument("--port", type=int, default=1883)
    parser.add_argument("--topic", default="reflowplate/set")
    parser.add_argument("--user", help="MQTT username; prompts for the password")
    args = parser.parse_args()

    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        parser.exit(1, 'Install paho-mqtt first: python -m pip install "paho-mqtt>=2,<3"\n')

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    if args.user:
        client.username_pw_set(args.user, getpass.getpass("MQTT password: "))
    try:
        client.connect(args.host, args.port, keepalive=10)
        client.loop_start()
        message = client.publish(args.topic, args.command, qos=1, retain=False)
        message.wait_for_publish(timeout=5)
        if not message.is_published():
            raise RuntimeError("broker did not confirm the command")
    except (OSError, RuntimeError, ValueError) as error:
        print(f"MQTT error: {error}", file=sys.stderr)
        return 1
    finally:
        client.loop_stop()
        client.disconnect()
    print(f"Sent to {args.topic}: {args.command}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
