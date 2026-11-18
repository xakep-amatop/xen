.. SPDX-License-Identifier: GPL-2.0-only

Arm virtual-timer tests
=======================

An Arm guest uses its virtual timer to wake up at a deadline. Before the
fix, an early timer IRQ could leave the timer masked. Linux could then
miss the next wake-up. The fix checks the timer status before Xen masks
the timer or raises the guest IRQ.

A push to ``test/arm-vtimer-ci`` selects jobs with ``vtimer`` in their
names. One job builds debug Xen with performance counters from the base
and pipeline commits. Another builds two Zephyr guests. Five tests boot
guests in QEMU with GICv3.

What changed in Xen?
--------------------

Each Xen test build compiles its own ``vtimer_interrupt()`` for a second
time with a model of an enabled, unmasked timer and a guest IRQ line.
Each row starts with a fresh timer.

====================  ==============================  ==============================
Timer status          Before the fix                  With the fix
====================  ==============================  ==============================
Deadline reached      Mask timer and raise guest IRQ  Mask timer and raise guest IRQ
Deadline still ahead  Mask timer and raise guest IRQ  Leave timer and IRQ alone
====================  ==============================  ==============================

The job counts writes to the timer, calls to the guest IRQ line and
increments of each Xen timer counter. With status clear, the old handler
counts a normal IRQ and the new one counts a spurious IRQ. The model then
advances the same timer to its deadline. It must raise no new guest IRQ
before the fix and one with the fix. The test sets the status bit directly,
so the result does not depend on runner speed.

How does QEMU produce an early IRQ?
-----------------------------------

#. The interrupt controller (GIC) and the timer have separate state.
   QEMU's device test interface, qtest, can mark the timer IRQ pending
   in the GIC while the guest runs. This leaves the timer deadline alone.
   When Xen handles that IRQ, it reads the timer's own status bit,
   ``ISTATUS``. For an enabled timer, a clear bit means its deadline is
   still ahead. This needs no GDB or breakpoint.
#. The runner waits for guest output so the request comes during the
   test. Linux reports that it is ready, waits for the runner to reply,
   then prints a marker as it starts the timed waits. Zephyr prints its
   ready marker after it starts a one-shot timer. The runner waits a short
   time after the relevant marker. The delay uses the runner's clock.
   The marker does not say what the timer status will be when Xen reads it.
#. QEMU replies ``OK`` when it accepts a request. At the end, the runner
   reads Xen's debug counters. A pass requires a positive count of timer
   IRQs that Xen handled with status clear. Linux uses the change between
   two counter readings. Zephyr uses the count since boot. Neither reading
   identifies an individual request.
   The guest checks below show whether time kept advancing. Finding the
   cause on physical hardware takes a separate measurement.

What do the guests check?
-------------------------

**Linux** uses the same kernel, rootfs and guest script in all three
jobs. One task sleeps 30 times. A busy loop keeps the pinned guest vCPU
runnable. Linux must report all 30 wake-ups in order and finish within
the 60-second test limit.

===================  ===============  ==========================================
Linux case           Xen image        What the job checks
===================  ===============  ==========================================
``before-selftest``  Before the fix   Early IRQ masks the timer in the model
``after-selftest``   Pipeline commit  Next deadline raises an IRQ in the model
``after-irq``        Pipeline commit  Timed waits finish after one QEMU request
===================  ===============  ==========================================

The first two jobs are required. When Linux says it is ready, the runner
sends ``Y`` to the Xen console. The test build runs a copy of its own
handler with the timer and guest IRQ line from the model above. Its counters
are private to the test. Whenever the handler raises the model IRQ line,
the test records the timer's ``ISTATUS`` bit. The runner forwards those
records and the end of each case to a test reader on the Linux console.
Both guests use the same reader. It counts one wake-up for each IRQ
with status set, following the Arm timer driver's check.

With an ordinary deadline both readers must count one wake-up. With an
early IRQ the old build must produce one ignored IRQ and zero wake-ups.
The fixed build must produce zero ignored IRQs and one wake-up. A lost
console record, a wrong count or a timeout fails the test. Each case ends
when the model reaches its deadline. Linux then runs its 30 real sleeps.

``qemu-arm64-vtimer-linux-gic`` runs ``after-irq`` automatically. Linux
waits at its ready marker while the runner reads the Xen counter. The
runner releases Linux and reads the counter again after the last wait.
If QEMU acknowledges the request, Linux finishes and the count stays flat,
the job reports ``INCONCLUSIVE`` (exit 77). GitLab shows a warning for
that result. A positive change is required for a pass. Missing output,
a console error or a Linux timeout fails the job.

**Zephyr v4.5.0** runs on the pipeline Xen image in two jobs. In
periodic mode the kernel ticks at regular intervals. In tickless mode
it sets its next deadline as needed. ``zephyr-4.5.0-arm64-vtimer`` builds
both modes from the probe source in ``zephyr/``, using the official
Zephyr CI base image on an Arm64 runner. The build script pins Zephyr
and Xenlib. It publishes binaries, configs and checksums under
``binaries/zephyr/``. Only the Zephyr test jobs need that artifact.
Each checks its config and makes 40 IRQ requests. A guest wrapper
reads ``ISTATUS`` before calling the standard timer handler. It must
see at least one timer IRQ and zero with status clear. The guest checks
its one-shot wake-up against the Arm counter, then runs a periodic
timer through 30 timed sleeps and requires at least 20 expiries.

Where are the results?
----------------------

Share the URL of the completed pipeline with the patch reply. The QEMU
jobs print ``CI_PIPELINE_URL`` in their logs and publish a JUnit result.
Open ``timer_progress`` for guest wake-ups and ``handler_before`` or
``handler_after`` for the model. The Linux test reader reports
``missing_wakeup_reproduced`` before the fix and ``wakeup_after_early_irq``
after it. Runs that request IRQs through the GIC also report
``xen_filter`` for the Xen counter.
The ``vtimer-results/`` artifacts include ``injections.log`` for QEMU
replies and ``console.log`` for guest output and Xen counters.

To run the guest tests locally, put the CI build artifacts in
``binaries/`` and run from the Xen tree. Linux needs the kernel and rootfs
in ``binaries/Image`` and ``binaries/rootfs.cpio.gz``. For Zephyr, unpack
the guest build artifact into the same tree, or run
``bash automation/scripts/arm-vtimer/build-zephyr.sh`` in its build image.
Give each test a fresh output directory.

.. code-block:: bash

    runner=automation/scripts/arm-vtimer/run.sh
    OUT=vtimer-before bash "$runner" linux before-selftest
    OUT=vtimer-after bash "$runner" linux after-selftest
    OUT=vtimer-irq bash "$runner" linux after-irq
    OUT=vtimer-periodic bash "$runner" zephyr periodic
    OUT=vtimer-tickless bash "$runner" zephyr tickless
