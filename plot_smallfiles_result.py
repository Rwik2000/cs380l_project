import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

# Load the CSV file uploaded by the user
file_path = "results_small_files_raw.csv"
df = pd.read_csv(file_path)

# Convert the "Time(s)" column to numerical values if needed
def parse_time(x):
    try:
        # Check if it's in the `0:00.64` format
        if ':' in x:
            parts = x.split(':')
            return float(parts[0]) * 60 + float(parts[1])  # Convert to total seconds
        else:
            return float(x)  # Already in seconds
    except Exception as e:
        print(f"Error parsing time: {x} - {e}")
        return None

# Apply parsing function
df['Time(s)'] = df['Time(s)'].apply(parse_time)

# Drop rows where parsing failed
df = df.dropna(subset=['Time(s)'])

# Group data by Tool and Operation
grouped = df.groupby(['Tool', 'Operation'])

# Calculate mean and standard deviation for Time(s)
stats = grouped['Time(s)'].agg(['mean', 'std']).reset_index()

# Plot 1: Average Time(s) for each Tool and Operation
plt.figure(figsize=(10, 6))
sns.barplot(data=stats, x='Tool', y='mean', hue='Operation', ci=None)
plt.title('Average Time (s) for Each Tool and Operation')
plt.ylabel('Average Time (s)')
plt.xlabel('Tool')
plt.xticks(rotation=45)
plt.legend(title='Operation')
plt.tight_layout()
# plt.savefig("/mnt/data/average_time_plot.png")
plt.show()

# Plot 2: Boxplot for Time(s) Distribution Across Tools
plt.figure(figsize=(10, 6))
sns.boxplot(data=df, x='Tool', y='Time(s)', hue='Operation')
plt.title('Time Distribution for Each Tool and Operation')
plt.ylabel('Time (s)')
plt.xlabel('Tool')
plt.xticks(rotation=45)
plt.legend(title='Operation')
plt.tight_layout()
# plt.savefig("/mnt/data/time_distribution_plot.png")
plt.show()

# Plot 3: Max Memory Usage Across Tools
memory_stats = grouped['Max Memory(KB)'].mean().reset_index()
plt.figure(figsize=(10, 6))
sns.barplot(data=memory_stats, x='Tool', y='Max Memory(KB)', hue='Operation', ci=None)
plt.title('Average Max Memory Usage for Each Tool and Operation')
plt.ylabel('Max Memory (KB)')
plt.xlabel('Tool')
plt.xticks(rotation=45)
plt.legend(title='Operation')
plt.tight_layout()
# plt.savefig("/mnt/data/max_memory_usage_plot.png")
plt.show()

# Plot 4: Line Chart for Time(s) per Trial
plt.figure(figsize=(12, 6))
for (tool, operation), group_data in df.groupby(['Tool', 'Operation']):
    plt.plot(group_data['Trial'], group_data['Time(s)'], marker='o', label=f"{tool} - {operation}")
plt.title('Time per Trial for Each Tool and Operation')
plt.ylabel('Time (s)')
plt.xlabel('Trial')
plt.legend(title='Tool and Operation', bbox_to_anchor=(1.05, 1), loc='upper left')
plt.tight_layout()
# plt.savefig("/mnt/data/time_per_trial_plot.png")
plt.show()
