#!/usr/bin/python3

import subprocess
import json
import sys
from paho.mqtt import client as mqtt_client
import random
import time

broker = "192.168.0.247"
port = 1883
topic = "fairylights/toggle"
client_id = f'python-mqtt-{random.randint(0, 1000)}'

proc = subprocess.Popen(['/usr/bin/rtl_433','-F','json','-q'],stdout=subprocess.PIPE)
last = time.time()
while True:
  line = proc.stdout.readline()
  if line != '':
    data = json.loads(line.rstrip())
    if data['model'] == "Akhan-100F14":
        if time.time() > last + 2:
          client = mqtt_client.Client(client_id)
          client.connect(broker, port)
          result = client.publish(topic, "Light toggle.")
          last = time.time()
    else:
      # Database gone away
      print(f"Dafuq? {json.dumps(data)}")
  else:
    break
  sys.stdout.flush()

cursor.close()
db.close()
