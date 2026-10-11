# CM1 oracle for the SC-01 / storm-domain experiments: gfortran + OpenMP + NetCDF, with the CM1 source cloned and built at image
# build time (NCAR/cm1 main; the commit is recorded in /opt/cm1/PINNED_COMMIT). Test oracle only: nothing in the engine needs it.
#   docker build -f tools/weather_lab/docker/Dockerfile -t orbit-cm1 tools/weather_lab
#   docker run --rm -v <host dir>:/work orbit-cm1 python3 /opt/cm1_lab.py run --cm1 /opt/cm1 --preset quick --out /work/cm1_quick --threads 4
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        gfortran make git ca-certificates python3 python3-numpy python3-netcdf4 libnetcdf-dev libnetcdff-dev \
    && rm -rf /var/lib/apt/lists/*
ARG CM1_REF=main
RUN git clone https://github.com/NCAR/cm1.git /opt/cm1 && cd /opt/cm1 && git checkout "$CM1_REF" && git log -1 --format='%H %cs' > /opt/cm1/PINNED_COMMIT
COPY cm1_lab.py /opt/cm1_lab.py
RUN python3 /opt/cm1_lab.py build --cm1 /opt/cm1
WORKDIR /work
