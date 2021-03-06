
FROM fluxrm/flux-core:centos8

# Default container user.
# "fluxuser" should match fluxrm/flux-core image
ARG USER=fluxuser
ARG UID=2100
ARG GID=2100

# UID for system "flux" user. Runs Flux instance
ARG FLUXUID=1000

ENV container docker

STOPSIGNAL SIGRTMIN+3

USER root

VOLUME [ "/sys/fs/cgroup" ]

# See: https://hub.docker.com/r/centos/systemd/dockerfile
RUN (cd /lib/systemd/system/sysinit.target.wants/; \
     for i in *; \
         do [ $i == systemd-tmpfiles-setup.service ] || rm -f $i; \
      done); \
    rm -f /lib/systemd/system/multi-user.target.wants/*;\
    rm -f /etc/systemd/system/*.wants/*;\
    rm -f /lib/systemd/system/local-fs.target.wants/*; \
    rm -f /lib/systemd/system/sockets.target.wants/*udev*; \
    rm -f /lib/systemd/system/sockets.target.wants/*initctl*; \
    rm -f /lib/systemd/system/basic.target.wants/*;\
    rm -f /lib/systemd/system/anaconda.target.wants/*;


RUN dnf install -y psmisc tmux \
 && dnf clean all

#
#  Install ttyd
RUN wget https://github.com/tsl0922/ttyd/releases/download/1.6.0/ttyd_linux.x86_64
RUN mv ttyd_linux.x86_64 /usr/bin/ttyd && chmod 755 /usr/bin/ttyd

#
#  Register ttyd service
RUN user=fluxuser && uid=$(id -u $user) && gid=$(id -g fluxuser) \
 && printf "#!/bin/sh\n"       >/bin/ttyd.sh \
 && printf "rm -f /var/run/nologin\n" >>/bin/ttyd.sh \
 && printf "/usr/bin/ttyd -o -p 7681 /bin/login $user\n" >>/bin/ttyd.sh \
 && printf "systemctl halt --no-block\n" >>/bin/ttyd.sh \
 && chmod 755 /bin/ttyd.sh \
 && printf "[Unit]\n" >/etc/systemd/system/ttyd.service \
 && printf "Description=ttyd service\n\n" >>/etc/systemd/system/ttyd.service \
 && printf "[Service]\n" >>/etc/systemd/system/ttyd.service \
 && printf "Type=simple\n" >>/etc/systemd/system/ttyd.service \
 && printf "ExecStart=/bin/ttyd.sh\n" >>/etc/systemd/system/ttyd.service \
 && printf "User=root\nGroup=root\n" >>/etc/systemd/system/ttyd.service \
 && printf "[Install]\nWantedBy=multi-user.target\n" >>/etc/systemd/system/ttyd.service

#
# Add flux user
#
RUN groupadd -g $FLUXUID flux \
 && useradd -g flux -u $FLUXUID -d /home/flux -m flux

# N.B.: "fluxuser" already exists in fluxrm/flux-core image

#
#  Add users besides fluxuser for mult-user testing
RUN for i in $(seq 1 5); do \
      user="user${i}"; \
      uid=$((${UID} + 100 + $i)); \
      printf "Adding ${user}\n"; \
      groupadd -g $uid $user; \
      useradd -g $user -u $uid -d /home/$user -m $user; \
    done

#  Copy in configuration
COPY imp.toml /etc/flux/imp/conf.d/imp.toml
COPY job-exec.toml /etc/flux/system/conf.d/exec.toml
COPY access.toml /etc/flux/system/conf.d/access.toml

RUN chmod 4755 /usr/libexec/flux/flux-imp \
 && systemctl enable flux.service \
 && systemctl enable munge.service \
 && systemctl enable ttyd.service

COPY f.txt /etc/motd
COPY entrypoint.sh /usr/local/sbin

WORKDIR /home/$USER
EXPOSE 7681
CMD [ "init" ]
