FROM ubuntu:latest

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

RUN apt-get update && apt-get install -y locales && rm -rf /var/lib/apt/lists/* \
    && localedef -i en_GB -c -f UTF-8 -A /usr/share/locale/locale.alias en_GB.UTF-8
ENV LANG en_GB.utf8

RUN apt-get update && apt-get -y upgrade && apt-get install -y rtl-433 python3-pip
RUN apt-get install -y python3-pymysql python3-paho-mqtt

COPY 433_monitor.py /usr/local/bin/433_monitor.py

RUN chmod 755 /usr/local/bin/433_monitor.py

CMD /usr/local/bin/433_monitor.py
