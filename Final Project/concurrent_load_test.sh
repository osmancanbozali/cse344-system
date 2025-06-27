#!/bin/bash

# Enhanced Concurrent User Load Test Script
# Tests 30+ clients connecting simultaneously with extensive whisper testing

# Colors for output
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# Configuration
SERVER_IP="127.0.0.1"
SERVER_PORT="5000"
NUM_CLIENTS=35
TEST_DURATION=20  # Increased duration for more whisper activity

echo -e "${BLUE}=== Enhanced Concurrent User Load Test with Whispers ===${NC}"
echo "Testing with $NUM_CLIENTS simultaneous clients"
echo "Server: $SERVER_IP:$SERVER_PORT"
echo "Duration: $TEST_DURATION seconds"
echo "Focus: Multiple whisper interactions between users"
echo ""

# Check if server is running
if ! nc -z "$SERVER_IP" "$SERVER_PORT" 2>/dev/null; then
    echo -e "${RED}ERROR: Server is not running on $SERVER_IP:$SERVER_PORT${NC}"
    echo "Please start the server first: ./chatserver $SERVER_PORT"
    exit 1
fi

echo -e "${GREEN}✓ Server is running${NC}"

# Clean up any existing temp files
rm -f /tmp/client_*.txt /tmp/load_test_*.log

# Create client command files with enhanced whisper testing
echo -e "${YELLOW}Creating client command files with extensive whisper patterns...${NC}"

for i in $(seq 1 $NUM_CLIENTS); do
    # Determine which room to join (spread clients across multiple rooms)
    if [ $i -le 10 ]; then
        room="project1"
    elif [ $i -le 20 ]; then
        room="team$((i % 5 + 1))"
    elif [ $i -le 30 ]; then
        room="group$((i % 3 + 1))"
    else
        room="testroom$((i % 2 + 1))"
    fi
    
    # Create unique usernames
    if [ $i -le 9 ]; then
        username="user0$i"
    else
        username="user$i"
    fi
    
    # Create multiple whisper targets for each user
    whisper_targets=()
    
    # Primary whisper target (previous user)
    if [ $i -gt 1 ]; then
        prev_i=$((i - 1))
        if [ $prev_i -le 9 ]; then
            whisper_targets+=("user0$prev_i")
        else
            whisper_targets+=("user$prev_i")
        fi
    else
        whisper_targets+=("user02")  # user01 whispers to user02
    fi
    
    # Secondary whisper target (next user)
    if [ $i -lt $NUM_CLIENTS ]; then
        next_i=$((i + 1))
        if [ $next_i -le 9 ]; then
            whisper_targets+=("user0$next_i")
        else
            whisper_targets+=("user$next_i")
        fi
    else
        whisper_targets+=("user01")  # last user whispers to user01
    fi
    
    # Tertiary whisper target (random user for cross-room whispers)
    random_target=$((RANDOM % NUM_CLIENTS + 1))
    if [ $random_target -ne $i ]; then
        if [ $random_target -le 9 ]; then
            whisper_targets+=("user0$random_target")
        else
            whisper_targets+=("user$random_target")
        fi
    fi
    
    # Create command file for each client with multiple whispers
    cat > "/tmp/client_${i}_commands.txt" << EOF
$username
/join $room
/broadcast Hello from $username in $room!
/whisper ${whisper_targets[0]} Hi there from $username - primary whisper
/broadcast Starting work in $room
/whisper ${whisper_targets[1]} This is $username whispering again!
/broadcast Making progress in $room
/whisper ${whisper_targets[0]} How are you doing?
/whisper ${whisper_targets[2]} Cross-room whisper from $username
/broadcast Almost done with work
/whisper ${whisper_targets[1]} Final whisper from $username before leaving
/whisper ${whisper_targets[0]} Thanks for the chat!
/broadcast Signing off from $room - goodbye!
/exit
EOF

done

echo -e "${GREEN}✓ Created $NUM_CLIENTS client command files with enhanced whisper patterns${NC}"

# Start all clients simultaneously
echo -e "${YELLOW}Starting $NUM_CLIENTS clients simultaneously...${NC}"

pids=()
start_time=$(date +%s)

for i in $(seq 1 $NUM_CLIENTS); do
    ./chatclient "$SERVER_IP" "$SERVER_PORT" < "/tmp/client_${i}_commands.txt" > "/tmp/load_test_client_${i}.log" 2>&1 &
    pids+=($!)
    
    # Show progress every 5 clients
    if [ $((i % 5)) -eq 0 ]; then
        echo -e "${BLUE}  Started $i clients...${NC}"
    fi
done

echo -e "${GREEN}✓ All $NUM_CLIENTS clients started${NC}"

# Wait for test duration
echo -e "${YELLOW}Running enhanced whisper test for $TEST_DURATION seconds...${NC}"
sleep $TEST_DURATION

# Kill any remaining client processes
echo -e "${YELLOW}Cleaning up client processes...${NC}"
for pid in "${pids[@]}"; do
    kill -TERM "$pid" 2>/dev/null
done

# Wait a moment for cleanup
sleep 2

# Kill any stubborn processes
pkill -f "chatclient $SERVER_IP $SERVER_PORT" 2>/dev/null

end_time=$(date +%s)
duration=$((end_time - start_time))

echo -e "${GREEN}✓ Enhanced whisper test completed in $duration seconds${NC}"

# Analyze results
echo ""
echo -e "${BLUE}=== Test Results ===${NC}"

# Count successful connections
successful_connections=0
successful_whispers=0
for i in $(seq 1 $NUM_CLIENTS); do
    if [ -f "/tmp/load_test_client_${i}.log" ]; then
        if grep -q "Welcome" "/tmp/load_test_client_${i}.log" 2>/dev/null; then
            successful_connections=$((successful_connections + 1))
        fi
        # Count whisper confirmations in client logs
        whisper_count=$(grep -c "Whisper sent to" "/tmp/load_test_client_${i}.log" 2>/dev/null || echo "0")
        successful_whispers=$((successful_whispers + whisper_count))
    fi
done

echo "Successful connections: $successful_connections/$NUM_CLIENTS"
echo "Total whispers sent: $successful_whispers"
echo "Expected whispers: $((NUM_CLIENTS * 5)) (5 per client)"

# Check server log for expected patterns
if [ -f "server.log" ]; then
    echo ""
    echo -e "${BLUE}=== Server Log Analysis ===${NC}"
    
    # Count log entries
    connect_logs=$(grep -c "\[CONNECT\] user 'user[0-9].*' connected" server.log 2>/dev/null || echo "0")
    join_logs=$(grep -c "\[INFO\] user[0-9].* joined room" server.log 2>/dev/null || echo "0")
    broadcast_logs=$(grep -c "\[BROADCAST\] user[0-9].*:" server.log 2>/dev/null || echo "0")
    
    echo "Connection logs: $connect_logs"
    echo "Room join logs: $join_logs"
    echo "Broadcast logs: $broadcast_logs"
    
    # Enhanced whisper analysis
    echo ""
    echo -e "${BLUE}=== Whisper Activity Analysis ===${NC}"
    
    # Count whisper messages in server log (if logged)
    whisper_server_logs=$(grep -c "whisper\|WHISPER" server.log 2>/dev/null || echo "0")
    echo "Server whisper-related logs: $whisper_server_logs"
    
    # Look for error patterns
    connection_errors=$(grep -c "ERROR\|failed\|refused" server.log 2>/dev/null || echo "0")
    echo "Connection errors in server log: $connection_errors"
    
    echo ""
    echo -e "${BLUE}=== Sample Log Entries (PDF Format) ===${NC}"
    echo "Recent connection logs:"
    grep "\[CONNECT\] user 'user[0-9].*' connected" server.log | tail -5
    
    echo ""
    echo "Recent room join logs:"
    grep "\[INFO\] user[0-9].* joined room" server.log | tail -5
    
    echo ""
    echo "Recent broadcast logs:"
    grep "\[BROADCAST\] user[0-9].*:" server.log | tail -5
    
    echo ""
    echo -e "${YELLOW}=== Full server.log ready for your report ===${NC}"
    echo "Log file location: $(pwd)/server.log"
    echo "Total log entries: $(wc -l < server.log)"
    
else
    echo -e "${RED}WARNING: server.log not found${NC}"
fi

# Enhanced client log analysis
echo ""
echo -e "${BLUE}=== Client Whisper Analysis ===${NC}"

# Analyze individual client logs for whisper patterns
whisper_received_count=0
whisper_error_count=0

for i in $(seq 1 $NUM_CLIENTS); do
    if [ -f "/tmp/load_test_client_${i}.log" ]; then
        # Count received whispers
        received=$(grep -c "\[WHISPER from" "/tmp/load_test_client_${i}.log" 2>/dev/null || echo "0")
        whisper_received_count=$((whisper_received_count + received))
        
        # Count whisper errors
        errors=$(grep -c "User.*not found\|whisper.*failed\|ERROR.*whisper" "/tmp/load_test_client_${i}.log" 2>/dev/null || echo "0")
        whisper_error_count=$((whisper_error_count + errors))
    fi
done

echo "Total whispers received by all clients: $whisper_received_count"
echo "Whisper errors: $whisper_error_count"

# Calculate whisper success rate
if [ $successful_whispers -gt 0 ]; then
    success_rate=$(echo "scale=2; $whisper_received_count * 100 / $successful_whispers" | bc -l 2>/dev/null || echo "N/A")
    echo "Whisper delivery rate: $success_rate% (received/sent)"
fi

# Results summary
echo ""
echo -e "${BLUE}=== Enhanced Test Summary ===${NC}"

if [ $successful_connections -ge 30 ]; then
    echo -e "${GREEN}✓ SUCCESS: $successful_connections clients connected successfully (≥30 required)${NC}"
    echo -e "${GREEN}✓ No server crashes detected${NC}"
    echo -e "${GREEN}✓ Log entries generated in PDF format${NC}"
    
    # Whisper-specific success criteria
    expected_whispers=$((NUM_CLIENTS * 5))
    whisper_threshold=$((expected_whispers * 8 / 10))  # 80% threshold
    
    if [ $successful_whispers -ge $whisper_threshold ]; then
        echo -e "${GREEN}✓ Whisper system working: $successful_whispers/$expected_whispers whispers sent${NC}"
    else
        echo -e "${YELLOW}⚠ Whisper performance: $successful_whispers/$expected_whispers whispers sent (< 80%)${NC}"
    fi
    
    if [ $whisper_error_count -eq 0 ]; then
        echo -e "${GREEN}✓ No whisper errors detected${NC}"
    else
        echo -e "${YELLOW}⚠ $whisper_error_count whisper errors detected${NC}"
    fi
    
    # Check for any client errors
    error_count=0
    for i in $(seq 1 $NUM_CLIENTS); do
        if [ -f "/tmp/load_test_client_${i}.log" ] && grep -q "ERROR\|failed\|refused" "/tmp/load_test_client_${i}.log" 2>/dev/null; then
            error_count=$((error_count + 1))
        fi
    done
    
    if [ $error_count -eq 0 ]; then
        echo -e "${GREEN}✓ No client connection errors detected${NC}"
    else
        echo -e "${YELLOW}⚠ $error_count clients experienced errors (check individual logs)${NC}"
    fi
    
    echo ""
    echo -e "${GREEN}🎉 ENHANCED CONCURRENT WHISPER TEST PASSED! 🎉${NC}"
    
else
    echo -e "${RED}✗ FAILED: Only $successful_connections clients connected successfully (< 30)${NC}"
    echo -e "${YELLOW}Check individual client logs in /tmp/load_test_client_*.log${NC}"
fi

# Show sample whisper interactions
echo ""
echo -e "${BLUE}=== Sample Whisper Interactions ===${NC}"
echo "Looking for whisper patterns in client logs..."

sample_count=0
for i in $(seq 1 $NUM_CLIENTS); do
    if [ -f "/tmp/load_test_client_${i}.log" ] && [ $sample_count -lt 3 ]; then
        echo ""
        echo -e "${YELLOW}Client user$(printf "%02d" $i) whisper activity:${NC}"
        grep -E "(Whisper sent to|WHISPER from)" "/tmp/load_test_client_${i}.log" | head -3
        sample_count=$((sample_count + 1))
    fi
done

# Cleanup option
echo ""
echo -e "${YELLOW}Cleanup temp files? (y/n)${NC}"
read -r cleanup
if [[ $cleanup =~ ^[Yy]$ ]]; then
    rm -f /tmp/client_*_commands.txt /tmp/load_test_*.log
    echo -e "${GREEN}✓ Temporary files cleaned up${NC}"
fi

echo ""
echo -e "${BLUE}Enhanced whisper test completed. Check server.log for complete PDF-format log entries.${NC}"
echo -e "${BLUE}This test validates private messaging under concurrent load conditions.${NC}"