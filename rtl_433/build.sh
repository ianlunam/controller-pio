docker pull ubuntu:latest
docker build -t myrtl_433 .
docker tag myrtl_433:latest repo.local:5000/myrtl_433:latest
docker push repo.local:5000/myrtl_433:latest
