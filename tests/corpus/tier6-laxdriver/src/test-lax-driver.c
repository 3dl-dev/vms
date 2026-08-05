/* Test the Load Average driver by reading from it.
 * You'll need to compile with /FLOAT=G_FLOAT to use the old driver,
 * and with /FLOAT=IEEE_FLOAT for the new, fixed-point driver.
 */

#define __NEW_STARLET 1
#include <descrip.h>
#include <iodef.h>
#include <stsdef.h>
#include <starlet.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {

#if __IEEE_FLOAT == 1
    $DESCRIPTOR (devnam, "LAX0:");
    uint32_t avgs[9] = { 0 };
#else
    $DESCRIPTOR (devnam, "LAV0:");
    float avgs[9] = { 0.0 };
#endif
    int status = EXIT_SUCCESS;
    unsigned short channel;

    /* assign the channel */
    status = sys$assign(&devnam, &channel, 0, NULL);
    if (!$VMS_STATUS_SUCCESS(status)) {
	fprintf(stderr, "test-lav-driver $assign err\n");
	return status;
    }

    /* handle the disable/enable update option (requires CMKRNL priv) */
    if (argc >= 2) {
	bool write_byte;
	if (!strcasecmp("-d", argv[1])) {
	    write_byte = true;
	} else if (!strcasecmp("-e", argv[1])) {
	    write_byte = false;
	} else {
	    fprintf(stderr, "error: ignoring unrecognized option '%s'\n", argv[1]);
	    fprintf(stderr, "use '-d' to disable updates and '-e' to enable them.\n");
	    status = EXIT_FAILURE;
	    goto cleanup;
	}

	/* write the byte */
	status = sys$qiow(0, channel, IO$_WRITEVBLK, NULL, NULL, 0,
			    &write_byte, sizeof(bool), 0, 0, 0, 0);

	if (!$VMS_STATUS_SUCCESS(status)) {
	    fprintf(stderr, "test-lav-driver $qiow err\n");
	}

	goto cleanup;	/* success: deassign and exit */
    }

    /* read the bytes */
    status = sys$qiow(0, channel, IO$_READVBLK, NULL, NULL, 0,
                      avgs, sizeof(avgs), 0, 0, 0, 0);
    if (!$VMS_STATUS_SUCCESS(status)) {
	fprintf(stderr, "test-lav-driver $qiow err\n");
	goto cleanup;
    }

    /* print the output */
#if __IEEE_FLOAT == 1
    static const double scale = (1.0 / (1 << 14));
    printf("load average:  %-12g  %-12g  %-12g\n",
	((double)avgs[0] * scale), ((double)avgs[1] * scale), ((double)avgs[2] * scale));
    printf("avg priority:  %-12g  %-12g  %-12g\n",
	((double)avgs[3] * scale), ((double)avgs[4] * scale), ((double)avgs[5] * scale));
    printf("av dsk q len:  %-12g  %-12g  %-12g\n",
	((double)avgs[6] * scale), ((double)avgs[7] * scale), ((double)avgs[8] * scale));
#else
    printf("load average:  %-12g  %-12g  %-12g\n", avgs[0], avgs[1], avgs[2]);
    printf("avg priority:  %-12g  %-12g  %-12g\n", avgs[3], avgs[4], avgs[5]);
    printf("av dsk q len:  %-12g  %-12g  %-12g\n", avgs[6], avgs[7], avgs[8]);
#endif

cleanup:
    /* deassign the channel and ignore return status */
    sys$dassgn(channel);

    /* return any VMS error status to CLI to print the error message */
    return status;
}
