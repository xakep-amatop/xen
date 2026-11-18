# SPDX-License-Identifier: GPL-2.0-only
function fail(reason) { if (!error) error = reason }
function report(test, reason, skipped) {
    printf "<testcase classname=\"%s\" name=\"%s\">", name, test
    if (skipped) {
        printf "<skipped message=\"%s\"/>", reason
        print reason > "/dev/stderr"
    } else if (reason) {
        printf "<failure>%s</failure>", reason
        print reason > "/dev/stderr"
        failed = 1
    }
    print "</testcase>"
    printf "%s\t%s\t%s\n", name, (skipped ? "SKIP" : reason ? "FAIL" : "PASS"), test > "tests.tsv"
}
{
    gsub(/\r/, ""); gsub(/\033\[[0-9;]*[[:alpha:]]/, "")
    if (/VTIMER_READY/) ready = 1
    if (selftest && /^\(XEN\) New guest IRQs at the next deadline in the model:/) print > "/dev/stderr"
    if (selftest && /^\(XEN\) vtimer selftest (before|after) (PASS|FAIL)$/) {
        selftests++
        print > "/dev/stderr"
        if (!ready || measure || $4 != selftest || $5 != "PASS") bad_selftest = 1
    }
    if (selftest && /^VTIMER_MODEL_RESULT /) {
        results[++intervals] = $0
        if (selftests != 1 || measure) bad_wait = 1
        print > "/dev/stderr"
    }
    if (/^VTIMER_MEASURE$/) measure = 1
    if (/^VTIMER_TICK_[0-9]+$/) {
        n = $0; sub(/^VTIMER_TICK_/, "", n)
        if (!measure || n + 0 != ticks) fail("Wrong Linux wait order")
        ticks++
    }
    if (/^VTIMER_LINUX_PASS$/) linux_pass = 1
    if (/Kernel panic|VTIMER_PROBE_FAIL/) fail("Guest failed")
    if (/VTIMER_PROBE_PASS/) {
        probes++
        if ($0 !~ /^\(d1\) VTIMER_PROBE_PASS expiries=[0-9]+ irqs=[0-9]+ spurious=[0-9]+$/)
            fail("Unexpected Zephyr result")
        split($3, e, "="); split($4, q, "="); split($5, s, "=")
        expiries = e[2] + 0; guest_irqs = q[2] + 0; spurious = s[2] + 0
    }
    if (/^\(XEN\) Virtual timer spurious interrupts[[:space:]]+TOTAL\[[[:space:]]*[0-9]+\]/) {
        value = $0; sub(/.*TOTAL\[[[:space:]]*/, "", value); filtered = value + 0
        counter_seen++
        if (name == "linux-after-irq") {
            if (counter_seen == 1) {
                baseline = filtered
                if (!ready || measure || linux_pass) bad_window = 1
            } else if (!measure || !linux_pass || ticks != 30) bad_window = 1
        }
    }
}
END {
    if (guest == "linux") {
        printf "Linux reported %d/30 timed waits.\n", ticks > "/dev/stderr"
        if (!ready || !measure || ticks != 30 || !linux_pass) fail("Linux did not finish all waits")
    } else {
        if (probes == 1)
            printf "Zephyr reported %d periodic expiries, %d timer IRQs, %d with ISTATUS clear.\n", \
                   expiries, guest_irqs, spurious > "/dev/stderr"
        if (!ready || probes != 1 || expiries < 20 || !guest_irqs || spurious)
            fail("Missing timer progress or spurious IRQ seen by Zephyr")
    }
    if (control_status) fail("Console test failed; see run.log")
    printf "<testsuites><testsuite name=\"%s\">", name
    report("timer_progress", error)
    if (selftest) {
        if (selftests != 1 || bad_selftest) test_error = "Missing or failed Xen handler selftest"
        report("handler_" selftest, test_error)
        early = (selftest == "before" ? "ignored=1 wakes=0" : "ignored=0 wakes=1")
        if (bad_wait || intervals != 2 || results[1] != "VTIMER_MODEL_RESULT control ignored=0 wakes=1" ||
            results[2] != ("VTIMER_MODEL_RESULT early " early))
            wait_error = "Linux did not receive the expected model timer events"
        report(selftest == "before" ? "missing_wakeup_reproduced" : "wakeup_after_early_irq", wait_error)
    }
    if (requests) {
        if (name == "linux-after-irq") {
            if (counter_seen != 2 || bad_window || filtered < baseline) {
                filter_error = "Missing or invalid Xen counter snapshots"
            } else {
                change = filtered - baseline
                printf "Xen spurious timer count: %d -> %d (change %d).\n", \
                       baseline, filtered, change > "/dev/stderr"
                if (!change) {
                    filter_error = "Xen recorded no new spurious timer IRQ during the test"
                    missed = !failed
                }
            }
        } else if (counter_seen) {
            printf "Xen spurious timer count for this run = %d.\n", filtered > "/dev/stderr"
            if (!filtered) filter_error = "Xen saw no timer IRQ with ISTATUS clear"
        } else {
            filter_error = "Xen did not print the spurious timer counter"
        }
        report("xen_filter", filter_error, missed)
    }
    print "</testsuite></testsuites>"
    exit (failed ? 1 : missed ? 77 : 0)
}
