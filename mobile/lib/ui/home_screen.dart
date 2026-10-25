import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:provider/provider.dart';

import '../core/dtmf_commands.dart';
import '../core/logger.dart';
import '../core/node_controller.dart';
import '../core/radio_session.dart';
import 'settings_screen.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  String _code = '';
  bool _starting = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      _bootstrap();
    });
  }

  Future<void> _bootstrap() async {
    setState(() => _starting = true);
    await _requestPermissions();
    if (!mounted) {
      return;
    }
    final controller = context.read<NodeController>();
    await controller.start();
    if (mounted) {
      setState(() => _starting = false);
    }
  }

  Future<void> _requestPermissions() async {
    try {
      await [
        Permission.bluetoothScan,
        Permission.bluetoothConnect,
        Permission.notification,
      ].request();
    } catch (_) {}
  }

  void _append(String digit) {
    setState(() => _code += digit);
  }

  Future<void> _send() async {
    final controller = context.read<NodeController>();
    final code = _code.isEmpty ? '0#' : _code;
    final ok = controller.engine?.submitDtmfCode(code) ?? false;
    if (!ok) {
      controller.logger.warning('Unknown DTMF code $code');
    }
    setState(() => _code = '');
  }

  @override
  Widget build(BuildContext context) {
    final controller = context.watch<NodeController>();
    final listenables = <Listenable>[controller, controller.logger];
    final engine = controller.engine;
    if (engine != null) {
      listenables.add(engine);
    }
    return ListenableBuilder(
      listenable: Listenable.merge(listenables),
      builder: (context, _) {
        final radio = controller.radio;
        final status = engine?.status ?? RadioStatus();
        final connected = radio?.connected ?? false;
        final configured = (controller.config?.mac ?? '').isNotEmpty;
        return Scaffold(
          appBar: AppBar(
            title: const Row(
              children: [
                Icon(Icons.radio, size: 22),
                SizedBox(width: 8),
                Text('BenshiNode'),
              ],
            ),
            actions: [
              IconButton(
                tooltip: 'Settings',
                icon: const Icon(Icons.settings),
                onPressed: () {
                  Navigator.of(context).push(
                    MaterialPageRoute<void>(
                      builder: (_) => const SettingsScreen(),
                    ),
                  );
                },
              ),
              IconButton(
                tooltip: controller.running ? 'Stop node' : 'Start node',
                icon: Icon(
                  controller.running ? Icons.stop_circle : Icons.play_circle,
                ),
                onPressed: _starting
                    ? null
                    : () async {
                        if (controller.running) {
                          await controller.stop();
                        } else {
                          await controller.start();
                        }
                      },
              ),
            ],
          ),
          body: ListView(
            padding: const EdgeInsets.all(14),
            children: [
              if (!configured)
                _SetupCard(
                  onOpenSettings: () {
                    Navigator.of(context).push(
                      MaterialPageRoute<void>(
                        builder: (_) => const SettingsScreen(),
                      ),
                    );
                  },
                ),
              _StatusCard(
                controller: controller,
                connected: connected,
                status: status,
              ),
              const SizedBox(height: 12),
              _QuickActions(controller: controller, connected: connected),
              const SizedBox(height: 12),
              _Keypad(
                code: _code,
                onDigit: _append,
                onClear: () => setState(() => _code = ''),
                onSend: _send,
              ),
              const SizedBox(height: 12),
              _LinkCard(controller: controller),
              const SizedBox(height: 12),
              _LogCard(controller: controller),
            ],
          ),
        );
      },
    );
  }
}

class _SetupCard extends StatelessWidget {
  const _SetupCard({required this.onOpenSettings});

  final VoidCallback onOpenSettings;

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text(
              'No radio selected',
              style: TextStyle(fontSize: 18, fontWeight: FontWeight.w600),
            ),
            const SizedBox(height: 6),
            const Text(
              'Pair the radio in Android Bluetooth settings, then choose it here.',
            ),
            const SizedBox(height: 12),
            FilledButton.tonalIcon(
              onPressed: onOpenSettings,
              icon: const Icon(Icons.bluetooth_searching),
              label: const Text('Select radio'),
            ),
          ],
        ),
      ),
    );
  }
}

class _StatusCard extends StatelessWidget {
  const _StatusCard({
    required this.controller,
    required this.connected,
    required this.status,
  });

  final NodeController controller;
  final bool connected;
  final RadioStatus status;

  @override
  Widget build(BuildContext context) {
    final engine = controller.engine;
    final config = controller.config;
    final channel = status.channelId >= 0
        ? status.channelId
        : (config?.channel ?? -1);
    final rssi = status.rssi.clamp(0, 15);
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Container(
                  width: 11,
                  height: 11,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: connected
                        ? const Color(0xFF33D17A)
                        : const Color(0xFFE05555),
                  ),
                ),
                const SizedBox(width: 8),
                Text(
                  connected ? 'Radio connected' : 'Radio offline',
                  style: const TextStyle(
                    fontSize: 17,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                const Spacer(),
                Text(
                  config?.callsign ?? '',
                  style: const TextStyle(fontWeight: FontWeight.w600),
                ),
              ],
            ),
            if (!connected) ...[
              const SizedBox(height: 8),
              Text(
                'Target: ${controller.lastRadioMac ?? config?.mac ?? '--'}',
                style: const TextStyle(
                  fontFamily: 'monospace',
                  color: Colors.white60,
                ),
              ),
              if (controller.lastRadioError != null)
                Padding(
                  padding: const EdgeInsets.only(top: 4),
                  child: Text(
                    'Last error: ${controller.lastRadioError}',
                    style: const TextStyle(color: Color(0xFFFFC857)),
                  ),
                ),
              Align(
                alignment: Alignment.centerLeft,
                child: TextButton.icon(
                  onPressed: controller.retryRadio,
                  icon: const Icon(Icons.refresh, size: 18),
                  label: const Text('Retry now'),
                ),
              ),
            ],
            const SizedBox(height: 12),
            Wrap(
              spacing: 18,
              runSpacing: 8,
              children: [
                _Metric(label: 'Channel', value: channel < 0 ? '--' : '$channel'),
                _Metric(
                  label: 'RSSI',
                  value: '${status.rssi}',
                  trailing: SizedBox(
                    width: 70,
                    child: LinearProgressIndicator(
                      value: rssi / 15,
                      minHeight: 5,
                    ),
                  ),
                ),
                _Metric(label: 'Power', value: status.powerOn ? 'On' : 'Off'),
                _Metric(label: 'TX', value: status.inTx ? 'Yes' : 'No'),
                _Metric(label: 'RX', value: status.inRx ? 'Yes' : 'No'),
                _Metric(
                  label: 'Squelch',
                  value: status.squelchOpen ? 'Open' : 'Closed',
                ),
                _Metric(
                  label: 'RX audio',
                  value: engine == null ||
                          DateTime.now()
                                  .difference(engine.lastRxAudioAt)
                                  .inSeconds >
                              3
                      ? '--'
                      : '${engine.lastRxDb.round()} dB',
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                _Pill(
                  label: engine?.parrotEnabled == true ? 'PARROT ON' : 'PARROT OFF',
                  active: engine?.parrotEnabled == true,
                ),
                const SizedBox(width: 8),
                _Pill(
                  label: engine?.txActive == true ? 'TX ACTIVE' : 'IDLE',
                  active: engine?.txActive == true,
                ),
                const SizedBox(width: 8),
                if (engine?.linkAudioActive == true)
                  const _Pill(label: 'LINK AUDIO', active: true),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _Metric extends StatelessWidget {
  const _Metric({required this.label, required this.value, this.trailing});

  final String label;
  final String value;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(
          label,
          style: const TextStyle(fontSize: 11, color: Colors.white60),
        ),
        Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              value,
              style: const TextStyle(
                fontSize: 16,
                fontWeight: FontWeight.w600,
              ),
            ),
            if (trailing != null) ...[
              const SizedBox(width: 8),
              trailing!,
            ],
          ],
        ),
      ],
    );
  }
}

class _Pill extends StatelessWidget {
  const _Pill({required this.label, required this.active});

  final String label;
  final bool active;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 5),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(20),
        color: active ? const Color(0xFF33D17A) : const Color(0xFF27332C),
      ),
      child: Text(
        label,
        style: TextStyle(
          fontSize: 11,
          fontWeight: FontWeight.w700,
          color: active ? Colors.black : Colors.white70,
        ),
      ),
    );
  }
}

class _QuickActions extends StatelessWidget {
  const _QuickActions({required this.controller, required this.connected});

  final NodeController controller;
  final bool connected;

  @override
  Widget build(BuildContext context) {
    final engine = controller.engine;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Text('Quick actions', style: TextStyle(fontWeight: FontWeight.w600)),
            const SizedBox(height: 10),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                _action('Help 0#', NodeCommand.help, engine, connected),
                _action('Identify 3#', NodeCommand.ident, engine, connected),
                _action(
                  engine?.parrotEnabled == true ? 'Parrot Off 1#' : 'Parrot On 1#',
                  NodeCommand.parrotToggle,
                  engine,
                  connected,
                ),
                _action('Reset 9#', NodeCommand.reset, engine, connected),
                FilledButton.tonal(
                  onPressed: connected
                      ? () async {
                          final ok = await controller.radio?.forceUnkey() ?? false;
                          if (!ok) {
                            controller.logger.warning('Unkey failed');
                          }
                        }
                      : null,
                  child: const Text('Unkey'),
                ),
                FilledButton.tonal(
                  onPressed: connected && engine != null
                      ? engine.testBeep
                      : null,
                  child: const Text('Test TX'),
                ),
                FilledButton.tonal(
                  onPressed: connected && engine != null
                      ? engine.testVoice
                      : null,
                  child: const Text('Test voice'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }

  Widget _action(
    String label,
    NodeCommand command,
    dynamic engine,
    bool connected,
  ) {
    return FilledButton.tonal(
      onPressed: connected && engine != null
          ? () => engine.runCommand(command)
          : null,
      child: Text(label),
    );
  }
}

class _Keypad extends StatelessWidget {
  const _Keypad({
    required this.code,
    required this.onDigit,
    required this.onClear,
    required this.onSend,
  });

  final String code;
  final void Function(String digit) onDigit;
  final VoidCallback onClear;
  final VoidCallback onSend;

  @override
  Widget build(BuildContext context) {
    final keys = <String>[
      '1', '2', '3',
      '4', '5', '6',
      '7', '8', '9',
      'Clear', '0', '#',
    ];
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                const Text('DTMF keypad', style: TextStyle(fontWeight: FontWeight.w600)),
                const Spacer(),
                Text(
                  code.isEmpty ? '0#' : code,
                  style: const TextStyle(
                    fontFamily: 'monospace',
                    fontSize: 20,
                    fontWeight: FontWeight.w700,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 10),
            GridView.count(
              crossAxisCount: 4,
              shrinkWrap: true,
              physics: const NeverScrollableScrollPhysics(),
              mainAxisSpacing: 8,
              crossAxisSpacing: 8,
              childAspectRatio: 2.6,
              children: [
                for (final key in keys)
                  FilledButton.tonal(
                    onPressed: key == 'Clear' ? onClear : () => onDigit(key),
                    child: Text(key, style: const TextStyle(fontSize: 16)),
                  ),
              ],
            ),
            const SizedBox(height: 8),
            SizedBox(
              width: double.infinity,
              child: FilledButton.icon(
                onPressed: onSend,
                icon: const Icon(Icons.send),
                label: const Text('Send command'),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _LinkCard extends StatelessWidget {
  const _LinkCard({required this.controller});

  final NodeController controller;

  @override
  Widget build(BuildContext context) {
    final config = controller.config;
    final server = config?.linkServer ?? '';
    final connected = controller.linkConnected;
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                const Text('BenshiNode link', style: TextStyle(fontWeight: FontWeight.w600)),
                const Spacer(),
                Text(
                  connected ? 'Connected' : (server.isEmpty ? 'Not configured' : 'Idle'),
                  style: TextStyle(
                    color: connected ? const Color(0xFF33D17A) : Colors.white60,
                    fontWeight: FontWeight.w600,
                  ),
                ),
              ],
            ),
            if (server.isNotEmpty) ...[
              const SizedBox(height: 6),
              Text(server, style: const TextStyle(color: Colors.white60)),
            ],
            const SizedBox(height: 10),
            Row(
              children: [
                FilledButton.tonal(
                  onPressed: connected ? null : () => controller.connectLink(),
                  child: const Text('Connect'),
                ),
                const SizedBox(width: 8),
                FilledButton.tonal(
                  onPressed: connected ? () => controller.disconnectLink() : null,
                  child: const Text('Disconnect'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _LogCard extends StatelessWidget {
  const _LogCard({required this.controller});

  final NodeController controller;

  @override
  Widget build(BuildContext context) {
    final entries = controller.logger.entries;
    final start = entries.length > 120 ? entries.length - 120 : 0;
    final lines = entries.sublist(start).reversed.toList(growable: false);
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(14),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                const Text('Live log', style: TextStyle(fontWeight: FontWeight.w600)),
                const Spacer(),
                IconButton(
                  tooltip: 'Copy log',
                  icon: const Icon(Icons.copy, size: 20),
                  onPressed: () {
                    Clipboard.setData(ClipboardData(text: lines.join('\n')));
                  },
                ),
                IconButton(
                  tooltip: 'Clear log',
                  icon: const Icon(Icons.delete_outline, size: 20),
                  onPressed: controller.logger.clear,
                ),
              ],
            ),
            SizedBox(
              height: 220,
              child: lines.isEmpty
                  ? const Center(child: Text('No log entries yet'))
                  : ListView.builder(
                      itemCount: lines.length,
                      itemBuilder: (context, index) {
                        final entry = lines[index];
                        return Text(
                          entry.formatted,
                          style: TextStyle(
                            fontFamily: 'monospace',
                            fontSize: 11,
                            color: entry.level == LogLevel.error
                                ? const Color(0xFFFF7676)
                                : entry.level == LogLevel.warning
                                    ? const Color(0xFFFFC857)
                                    : Colors.white70,
                          ),
                        );
                      },
                    ),
            ),
          ],
        ),
      ),
    );
  }
}
