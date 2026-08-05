From:	CRDGW2::CRDGW2::MRGATE::"SMTP::CRVAX.SRI.COM::RELAY-INFO-VAX"  2-AUG-1990 07:55:54.08
To:	MRGATE::"ARISIA::EVERHART"
CC:	
Subj:	Re: LOAD_AVERAGE

Received:  by crdgw1.ge.com (5.57/GE 1.70)
	 id AA07958; Tue, 31 Jul 90 19:43:21 EDT
Received: From TGV.COM by CRVAX.SRI.COM with TCP; Tue, 31 JUL 90 13:10:20 PDT
Date:     Tue, 31 Jul 90 11:41:51 PDT
From: adelman@TGV.COM (Kenneth Adelman)
Reply-To: Adelman@TGV.COM (Kenneth Adelman)
Message-Id: <900731113036.2a401734@TGV.COM>
Subject:  Re: LOAD_AVERAGE
To: wilton%hg.uleth.ca@TGV.COM
Cc: info-vax@sri.com

>  In the VMS 5.2 Release notes, section 3.24.2 there is a description of
> the LAT dynamic service rating algorithm.  On of the factors in the equation
> is a number called LOAD_AVERAGE which is described as "a moving average of
> the number of computable processes waiting in the VMS scheduler queues."
> This sounds like it could be a very useful number to be able to display.  Does
> anybody know if this is available through a system call or something similar?
> I see no mention of LOAD_AVERAGE in the master index to the doc set.	When we
> switched from TOPS-20 to VMS a few years back, we really missed the load
> average number on the CTRL-T line.  So if there is some way to get at this
> LOAD_AVERAGE, I would like to hear about it.	Thanks for any help.

    Here is a VMS driver you can load which will keep track of the
load average in a way you can retrieve it. The algorithm used by it is
not necessarily the same as LAT, but closer to TOPS. To read the
load average, $ASSIGN or open a channel to the device _LAV0: and
read 36 bytes from it. The first 12 bytes are three "F"-type floats
which are the 1, 5, and 15 minute load averages respectively. The
next 12 bytes are the 1, 5, and 15 minute average blocking priority (if
your process is below the average blocking priority you aren't going to
get much CPU, if above it you will get a lot). The last three are the
1, 5, and 15 minute averages of the longest I/O queue length on any disk,
a decent indication of how backed up your I/O system is.

							Kenneth Adelman
							TGV
